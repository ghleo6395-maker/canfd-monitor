"""Bounded metadata-only DBC / BUSMASTER DBF import via canmatrix.

This module never writes a transmit entry and never sends a frame. Database
files describe layouts, not captured payloads: initial draft bytes are zero.
"""
from contextlib import redirect_stderr, redirect_stdout
from decimal import Decimal, InvalidOperation
import io
import logging
from pathlib import PurePath
import re
import threading

MAX_DATABASE_BYTES = 2 * 1024 * 1024
MAX_DATABASE_FRAMES = 4096
MAX_FRAME_SIGNALS = 256
MAX_TOTAL_SIGNALS = 16384
MAX_LINE_BYTES = 32768
DEFAULT_CYCLE_MS = 100
_ENCODINGS = ("utf-8-sig", "gb18030", "cp1252")
_ENCODING_ALIASES = {"utf-8":"utf-8-sig", "utf-8-sig":"utf-8-sig", "gbk":"gb18030", "gb18030":"gb18030", "cp1252":"cp1252", "iso-8859-1":"iso-8859-1"}
_PARSE_LOCK = threading.Lock()


class DatabaseImportError(ValueError):
    """User-facing import rejection. The caller must preserve the old catalog."""


class _BoundedOutput(io.StringIO):
    def write(self, text):
        remaining = 4096 - self.tell()
        if remaining > 0:
            super().write(text[:remaining])
        return len(text)


class _Diagnostics(logging.Handler):
    def __init__(self):
        super().__init__(logging.WARNING)
        self.errors = []
        self.warnings = []

    def emit(self, record):
        target = self.errors if record.levelno >= logging.ERROR else self.warnings
        if len(target) < 12:
            target.append(record.getMessage()[:240])


def _decode(raw, encoding):
    if not isinstance(encoding, str):
        raise DatabaseImportError("文本编码参数无效")
    if encoding != "auto" and encoding not in _ENCODING_ALIASES:
        raise DatabaseImportError("编码支持 auto、UTF-8、GB18030/GBK、CP1252 或 ISO-8859-1")
    candidates = _ENCODINGS if encoding == "auto" else (_ENCODING_ALIASES[encoding],)
    for candidate in candidates:
        try:
            text = raw.decode(candidate)
        except UnicodeDecodeError:
            continue
        if "\0" in text or any(ord(c) < 32 and c not in "\t\r\n" for c in text):
            raise DatabaseImportError("文件含二进制或控制字符；仅支持文本 DBC / BUSMASTER CAN DBF，请导出 UTF-8 文本")
        if any(len(line.encode("utf-8")) > MAX_LINE_BYTES for line in text.splitlines()):
            raise DatabaseImportError("数据库某行过长，单行上限为 32 KiB")
        return text, "utf-8" if candidate == "utf-8-sig" else candidate
    raise DatabaseImportError("文本解码失败，请选择原文件编码或重新导出为 UTF-8")


def _id(identifier, extended, line):
    if identifier < 0 or identifier > (0x1FFFFFFF if extended else 0x7FF):
        raise DatabaseImportError(f"第 {line} 行报文 ID 超出{'扩展 29' if extended else '标准 11'}位范围")


def _length(length, line):
    if length not in (0,1,2,3,4,5,6,7,8,12,16,20,24,32,48,64):
        raise DatabaseImportError(f"第 {line} 行报文长度不是 CAN / CAN FD 可发送的字节长度")


def _preflight_dbc(text, warnings):
    if "[START_MSG]" in text or "[DATABASE_VERSION]" in text:
        raise DatabaseImportError("文件内容是 BUSMASTER DBF，请使用 .dbf 扩展名")
    supported = {"VERSION", "NS_", "BS_", "BU_", "BO_", "SG_", "BO_TX_BU_", "CM_", "BA_DEF_", "BA_DEF_DEF_", "BA_", "VAL_", "VAL_TABLE_", "SIG_GROUP_", "SIG_VALTYPE_", "SG_MUL_VAL_", "EV_"}
    optional = {"NS_DESC_", "CAT_DEF_", "CAT_", "FILTER", "ENVVAR_DATA_", "SGTYPE_", "SGTYPE_VAL_", "BA_DEF_SGTYPE_", "BA_SGTYPE_", "SIG_TYPE_REF_", "BA_DEF_REL_", "BA_REL_", "BA_DEF_DEF_REL_", "BU_SG_REL_", "BU_EV_REL_", "BU_BO_REL_"}
    frames, current, namespace, comment = [], None, False, False
    for line_number, raw_line in enumerate(text.splitlines(), 1):
        line = raw_line.strip()
        if not line or line.startswith("//"):
            continue
        if comment:
            if re.search(r'(?<!\\)"\s*;\s*$', line):
                comment = False
            continue
        token_match = re.match(r"^([A-Z][A-Z0-9_]*)(?=\s|:|$)", line)
        token = token_match.group(1) if token_match else ""
        if namespace and re.fullmatch(r"[A-Z][A-Z0-9_]*", line):
            continue
        if token == "NS_":
            namespace = True
            continue
        namespace = False
        if token not in supported | optional:
            raise DatabaseImportError(f"第 {line_number} 行不是可识别的 DBC 声明")
        if token in optional:
            warnings.add("部分 DBC 辅助声明不参与本轮发送草稿（例如关系、分类或信号类型定义）；原文件未修改")
        if token == "CM_":
            quote_count = len(re.findall(r'(?<!\\)"', line))
            if quote_count % 2:
                comment = True
        if token == "BO_":
            match = re.fullmatch(r"BO_\s+(\d+)\s+(\S+)\s*:\s*(\d+)\s+\S+(?:\s*)", line)
            if not match:
                raise DatabaseImportError(f"第 {line_number} 行 BO_ 报文声明损坏")
            encoded_id, name, size = int(match[1]), match[2], int(match[3])
            if encoded_id > 0xFFFFFFFF:
                raise DatabaseImportError(f"第 {line_number} 行 DBC 报文 ID 编码超出 32 位范围")
            placeholder = name == "VECTOR__INDEPENDENT_SIG_MSG" and encoded_id == 0xC0000000
            extended = bool(encoded_id & 0x80000000)
            identifier = encoded_id & 0x7FFFFFFF if extended else encoded_id
            if not placeholder:
                _id(identifier, extended, line_number)
                _length(size, line_number)
            else:
                warnings.add("已跳过 VECTOR__INDEPENDENT_SIG_MSG 独立信号占位消息，它不是可发送的 CAN 报文")
            current = {"id":identifier, "extended":extended, "size":size, "signals":0, "line":line_number, "placeholder":placeholder}
            frames.append(current)
            if len(frames) > MAX_DATABASE_FRAMES:
                raise DatabaseImportError("数据库超过 4096 帧上限")
        elif token == "SG_":
            if current is None:
                raise DatabaseImportError(f"第 {line_number} 行信号没有所属报文")
            current["signals"] += 1
            if current["signals"] > MAX_FRAME_SIGNALS:
                raise DatabaseImportError("单帧信号超过 256 个上限")
    if comment:
        raise DatabaseImportError("DBC 注释未正常结束，文件可能被截断")
    return [frame for frame in frames if not frame["placeholder"]]


def _preflight_dbf(text):
    if not re.search(r"^\s*\[DATABASE_VERSION\]\s+", text, re.M) or not re.search(r"^\s*\[START_MSG\]", text, re.M):
        raise DatabaseImportError("此 .dbf 不是可识别的 BUSMASTER CAN 文本数据库；普通 dBase/表格 DBF 不支持")
    protocol = re.search(r"^\s*\[PROTOCOL\]\s+([^\r\n]+)", text, re.M)
    if not protocol or protocol[1].strip().upper() != "CAN":
        raise DatabaseImportError("当前 DBF 仅支持 BUSMASTER 的 [PROTOCOL] CAN；其他协议或 CAN FD DBF 请先导出为标准 DBC")
    frames, current, declared = [], None, None
    known = {"DATABASE_VERSION", "PROTOCOL", "BUSMASTER_VERSION", "NUMBER_OF_MESSAGES", "START_MSG", "END_MSG", "START_SIGNALS", "VALUE_DESCRIPTION", "NODE", "START_DESC", "END_DESC", "START_DESC_MSG", "END_DESC_MSG", "START_DESC_NODE", "END_DESC_NODE", "START_DESC_SIG", "END_DESC_SIG", "START_PARAM", "END_PARAM", "START_PARAM_NET", "END_PARAM_NET", "START_PARAM_NODE", "END_PARAM_NODE", "START_PARAM_MSG", "END_PARAM_MSG", "START_PARAM_SIG", "END_PARAM_SIG", "START_PARAM_VAL", "END_PARAM_VAL", "START_PARAM_NODE_VAL", "END_PARAM_NODE_VAL", "START_PARAM_NET_VAL", "END_PARAM_NET_VAL", "START_PARAM_MSG_VAL", "END_PARAM_MSG_VAL", "START_PARAM_SIG_VAL", "END_PARAM_SIG_VAL"}
    in_metadata = False
    for line_number, raw_line in enumerate(text.splitlines(), 1):
        line = raw_line.strip()
        if not line or line.startswith("//"):
            continue
        marker = re.match(r"^\[([A-Z_]+)\]", line)
        if not marker:
            if not in_metadata:
                raise DatabaseImportError(f"第 {line_number} 行不是可识别的 BUSMASTER DBF 记录")
            continue
        token = marker[1]
        if token not in known:
            raise DatabaseImportError(f"第 {line_number} 行 DBF 区段不受支持：{token}")
        if token.startswith("START_DESC") or token.startswith("START_PARAM"):
            in_metadata = True
        if token in {"END_DESC", "END_PARAM"}:
            in_metadata = False
        value = line[marker.end():].strip()
        if token == "NUMBER_OF_MESSAGES":
            try:
                declared = int(value)
            except ValueError as error:
                raise DatabaseImportError(f"第 {line_number} 行报文数量损坏") from error
        elif token == "START_MSG":
            if current is not None:
                raise DatabaseImportError(f"第 {line_number} 行新报文出现前，上一报文缺少 [END_MSG]")
            fields = [field.strip() for field in value.split(",")]
            try:
                name, identifier, size, signals = fields[0], int(fields[1]), int(fields[2]), int(fields[3])
                kind = fields[5] if len(fields) > 5 else "S"
                if len(fields) < 5 or not name or kind not in {"S", "X"}:
                    raise ValueError
            except (IndexError, ValueError) as error:
                raise DatabaseImportError(f"第 {line_number} 行 DBF 报文声明损坏") from error
            if identifier < 0 or identifier > 0xFFFFFFFF:
                raise DatabaseImportError(f"第 {line_number} 行 DBF 报文 ID 无效")
            raw_id = identifier
            extended = kind == "X" or bool(identifier & 0x80000000)
            identifier = identifier & 0x7FFFFFFF if identifier & 0x80000000 else identifier
            _id(identifier, extended, line_number)
            _length(size, line_number)
            if size > 8:
                raise DatabaseImportError(f"第 {line_number} 行 DBF 长帧未明确标注受支持的 CAN FD 格式，请导出为 DBC")
            if signals < 0 or signals > MAX_FRAME_SIGNALS:
                raise DatabaseImportError("单帧信号数量必须在 0 到 256 范围内")
            current = {"id":identifier, "raw_id":raw_id, "extended":extended, "size":size, "signals":0, "declared_signals":signals, "line":line_number}
            frames.append(current)
            if len(frames) > MAX_DATABASE_FRAMES:
                raise DatabaseImportError("数据库超过 4096 帧上限")
        elif token == "START_SIGNALS":
            if current is None:
                raise DatabaseImportError(f"第 {line_number} 行信号没有所属报文")
            current["signals"] += 1
        elif token == "END_MSG":
            if current is None or current["signals"] != current["declared_signals"]:
                raise DatabaseImportError(f"第 {line_number} 行 DBF 声明的信号数量与实际记录不一致")
            current = None
    if current is not None:
        raise DatabaseImportError("DBF 最后一条报文缺少 [END_MSG]，文件可能被截断")
    if declared is not None and declared != len(frames):
        raise DatabaseImportError("DBF 声明的报文数量与实际记录不一致")
    return frames


def _dbf_parser_text(text, specs):
    # BUSMASTER writes a 29-bit ID plus an explicit X field. canmatrix 1.3.3
    # validates the number as standard before applying X. Convert only the
    # parser's copy to its compound-ID convention, preserving the source ID.
    extended_ids = {spec["raw_id"] for spec in specs if spec["extended"]}
    standard_ids = {spec["raw_id"] for spec in specs if not spec["extended"]}
    result, metadata = [], False
    for raw_line in text.splitlines():
        line = raw_line.strip()
        if line.startswith("[START_MSG]"):
            fields = raw_line.split("]", 1)[1].strip().split(",")
            identifier = int(fields[1])
            kind = fields[5].strip() if len(fields) > 5 else "S"
            if kind == "X" or identifier & 0x80000000:
                fields[1] = str(identifier | 0x80000000)
            raw_line = "[START_MSG] " + ",".join(fields)
        elif line.startswith(("[START_DESC_MSG]", "[START_DESC_SIG]", "[START_PARAM_MSG_VAL]", "[START_PARAM_SIG_VAL]")):
            metadata = True
        elif line.startswith("[END_"):
            metadata = False
        elif metadata:
            match = re.match(r"^(\s*)(\d+)([ ,]+)([SX])(?:\b|,)", raw_line)
            if match:
                identifier = int(match[2])
                if match[4] == "X" or (identifier in extended_ids and identifier not in standard_ids):
                    raw_line = raw_line[:match.start(2)] + str(identifier | 0x80000000) + raw_line[match.end(2):]
        result.append(raw_line)
    return "\n".join(result) + "\n"


def _number(value):
    if value is None:
        return None
    try:
        number = Decimal(str(value))
    except InvalidOperation as error:
        raise DatabaseImportError("数据库含无效信号数值") from error
    if not number.is_finite():
        raise DatabaseImportError("数据库含非有限信号数值")
    return str(number)


def _parse_database(name, raw_bytes, encoding="auto"):
    """Return bounded, validated metadata; raise DatabaseImportError atomically."""
    if not isinstance(name, str) or not name or len(name.encode("utf-8")) > 240 or any(c in name for c in "/\\\0") or any(ord(c) < 32 for c in name):
        raise DatabaseImportError("数据库文件名无效，请只提供 .dbc 或 .dbf 文件名")
    file_format = PurePath(name).suffix.lower()[1:]
    if file_format not in {"dbc", "dbf"}:
        raise DatabaseImportError("发送数据库仅支持 .dbc 和 BUSMASTER CAN .dbf；日志回放请使用 candump .log/.txt")
    if not isinstance(raw_bytes, bytes) or not raw_bytes or len(raw_bytes) > MAX_DATABASE_BYTES:
        raise DatabaseImportError("数据库不能为空且不能超过 2 MiB")
    text, selected_encoding = _decode(raw_bytes, encoding)
    warnings = {"数据库仅描述报文与信号，不含实际报文数据；导入发送草稿的数据初始化为 00，不自动编码信号初值或发送"}
    specs = _preflight_dbc(text, warnings) if file_format == "dbc" else _preflight_dbf(text)
    if not specs:
        raise DatabaseImportError("数据库没有可发送的 CAN 报文")
    if sum(spec["signals"] for spec in specs) > MAX_TOTAL_SIGNALS:
        raise DatabaseImportError("数据库总信号数超过 16384 上限")
    try:
        from canmatrix.formats import dbc, dbf
    except ImportError as error:
        raise DatabaseImportError("未安装数据库解析模块，请在 fw 下执行 .venv-sim/bin/pip install -r sim/web/requirements.txt 并使用该虚拟环境启动模拟器") from error
    diagnostics, printed = _Diagnostics(), _BoundedOutput()
    logger = logging.getLogger("canmatrix")
    with _PARSE_LOCK:
        logger.addHandler(diagnostics)
        try:
            with redirect_stdout(printed), redirect_stderr(printed):
                module = dbc if file_format == "dbc" else dbf
                parser_text = text if file_format == "dbc" else _dbf_parser_text(text, specs)
                matrix = module.load(io.BytesIO(parser_text.encode("utf-8")), **{file_format + "ImportEncoding":"utf-8", "dbcImportCommentEncoding":"utf-8"})
        except Exception as error:
            raise DatabaseImportError("数据库解析失败，文件结构、字段或编码无效；原有发送列表保持不变") from error
        finally:
            logger.removeHandler(diagnostics)
    if diagnostics.errors or printed.getvalue().strip() or getattr(matrix, "load_errors", None):
        match = re.search(r"line no:\s*(\d+)", printed.getvalue())
        line_text = f"（第 {match[1]} 行）" if match else ""
        raise DatabaseImportError("数据库解析发现错误" + line_text + "，未导入任何报文；请修复原文件")
    if diagnostics.warnings:
        warnings.add("解析器警告：" + diagnostics.warnings[0])
    bus_type = str(matrix.attributes.get("BusType", "CAN")).strip().upper().replace("_", " ")
    if bus_type not in {"CAN", "CAN FD", "CANFD", ""}:
        raise DatabaseImportError("该数据库声明的总线不是 CAN / CAN FD")
    source_frames = [frame for frame in matrix.frames if frame.name != "VECTOR__INDEPENDENT_SIG_MSG"]
    if len(source_frames) != len(specs):
        raise DatabaseImportError("解析后的报文数量与原文件不一致，拒绝部分导入")
    frames, seen = [], set()
    for index, (frame, spec) in enumerate(zip(source_frames, specs)):
        identifier, extended = frame.arbitration_id.id, bool(frame.arbitration_id.extended)
        if (identifier, extended, frame.size) != (spec["id"], spec["extended"], spec["size"]):
            raise DatabaseImportError(f"第 {spec['line']} 行报文解析结果不一致，拒绝部分导入")
        if (identifier, extended) in seen:
            raise DatabaseImportError("数据库含重复的同格式报文 ID，请先明确要发送的唯一报文定义")
        seen.add((identifier, extended))
        format_value = frame.attribute("VFrameFormat", db=matrix, default="")
        format_define = matrix.frame_defines.get("VFrameFormat")
        if format_define is not None and format_define.type == "ENUM" and str(format_value).isdigit():
            values = format_define.values
            if int(format_value) >= len(values):
                raise DatabaseImportError(f"报文 {frame.name} 的帧格式枚举无效")
            format_value = values[int(format_value)]
        frame_format = str(format_value)
        if any(token in frame_format.upper() for token in ("LIN", "FLEXRAY", "ETHERNET", "CANXL", "CAN_XL")):
            raise DatabaseImportError("数据库含非 CAN / CAN FD 或 CAN XL 报文")
        if file_format == "dbf" and any(token in frame_format.upper() for token in ("_FD", "CANFD", "CAN FD")):
            raise DatabaseImportError("此 DBF 的 CAN FD 扩展尚不支持，请导出为标准 DBC")
        fd = bool(frame.is_fd) or "_FD" in frame_format.upper()
        if frame.size > 8 and not fd:
            raise DatabaseImportError(f"报文 {frame.name} 超过 8 字节但没有明确 CAN FD 声明")
        brs_raw = str(frame.attribute("CANFD_BRS", db=matrix, default="0") or "0").strip().lower()
        if brs_raw not in {"0", "1", "false", "true", "no", "yes"}:
            raise DatabaseImportError(f"报文 {frame.name} 的 BRS 属性无效")
        brs = brs_raw in {"1", "true", "yes"}
        if brs and not fd:
            raise DatabaseImportError(f"报文 {frame.name} 声明了 BRS 但不是 CAN FD")
        cycle_raw = frame.attributes.get("GenMsgCycleTime", frame.cycle_time or frame.attribute("GenMsgCycleTime", db=matrix) or 0)
        try:
            cycle_value = Decimal(str(cycle_raw))
            if not cycle_value.is_finite() or cycle_value != cycle_value.to_integral_value() or cycle_value < 0:
                raise ValueError
            cycle = int(cycle_value)
        except (ValueError, InvalidOperation) as error:
            raise DatabaseImportError(f"报文 {frame.name} 的周期不是有效的非负整数毫秒") from error
        if cycle <= 0 or cycle > 65535:
            warnings.add("未指定、事件型或超出当前发送条目范围的周期暂填 100 ms；可在发送前自行修改")
            cycle = DEFAULT_CYCLE_MS
        if len(frame.signals) != spec["signals"]:
            raise DatabaseImportError(f"报文 {frame.name} 的信号数量与原文件不一致，拒绝部分导入")
        signals = []
        for signal in frame.signals:
            if not 0 < int(signal.size) <= 512 or not 0 <= int(signal.start_bit) or int(signal.start_bit) + int(signal.size) > frame.size * 8:
                raise DatabaseImportError(f"报文 {frame.name} 的信号 {signal.name} 超出数据边界")
            signals.append({"name":str(signal.name)[:160], "start_bit":int(signal.get_startbit(bit_numbering=1)), "length":int(signal.size), "byte_order":"little" if signal.is_little_endian else "big", "signed":bool(signal.is_signed), "unit":str(signal.unit or "")[:80], "factor":_number(signal.factor), "offset":_number(signal.offset), "minimum":_number(signal.min), "maximum":_number(signal.max)})
        frames.append({"key":f"frame-{index}", "name":str(frame.name)[:160], "id":identifier, "extended":extended, "fd":fd, "brs":brs, "length":int(frame.size), "cycle_ms":cycle, "data":" ".join(["00"] * int(frame.size)), "signals":signals})
    if selected_encoding != "utf-8":
        warnings.add(f"已按 {selected_encoding.upper()} 解码；若名称乱码，请手动选择原始编码")
    return {"name":name, "format":file_format, "format_label":"Vector DBC" if file_format == "dbc" else "BUSMASTER DBF", "encoding":selected_encoding, "frames":frames, "warnings":sorted(warnings)}


def parse_database(name, raw_bytes, encoding="auto"):
    """Return a complete catalog or a user-facing error, never partial data."""
    try:
        return _parse_database(name, raw_bytes, encoding)
    except DatabaseImportError:
        raise
    except Exception as error:
        raise DatabaseImportError("数据库字段无效，未导入任何报文；请检查原文件") from error
