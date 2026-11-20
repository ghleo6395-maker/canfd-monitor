"""Real canmatrix parsing, strict input validation and bounded metadata tests."""
from contextlib import redirect_stderr, redirect_stdout
import importlib.util
import io
import json
from pathlib import Path
import unittest
from unittest import mock

ROOT = Path(__file__).parents[1]
spec = importlib.util.spec_from_file_location("database_import", ROOT / "sim/web/database_import.py")
database = importlib.util.module_from_spec(spec)
spec.loader.exec_module(database)
parse_database = database.parse_database
DatabaseImportError = database.DatabaseImportError
DBC = (ROOT / "sim/web/examples/review.dbc").read_text()
DBF = (ROOT / "sim/web/examples/review.dbf").read_text()


class DatabaseImportTests(unittest.TestCase):
    def parse_dbc(self, text=DBC, encoding="utf-8", selected="auto"):
        return parse_database("review.dbc", text.encode(encoding), selected)

    def test_real_dbc_classic_extended_fd_brs_signals_and_cycle(self):
        result = self.parse_dbc()
        frames = result["frames"]
        self.assertEqual(result["format"], "dbc")
        self.assertEqual(result["encoding"], "utf-8")
        self.assertEqual([(f["id"], f["extended"], f["length"], f["fd"], f["brs"], f["cycle_ms"]) for f in frames],
                         [(0x123, False, 8, False, False, 100), (0x18FF50E5, True, 8, False, False, 500), (0x3A1, False, 64, True, True, 20)])
        self.assertEqual(frames[0]["signals"][0]["factor"], "0.1")
        self.assertEqual(frames[0]["signals"][0]["byte_order"], "little")
        self.assertEqual(frames[2]["signals"][-1]["start_bit"], 504)
        self.assertEqual(frames[2]["data"].split(), ["00"] * 64)
        self.assertTrue(any("不含实际报文数据" in warning for warning in result["warnings"]))
        self.assertEqual(json.loads(json.dumps(result, allow_nan=False))["frames"][0]["id"], 0x123)

    def test_brs_is_not_invented_from_fd(self):
        result = self.parse_dbc(DBC.replace('BA_ "CANFD_BRS" BO_ 929 1;', 'BA_ "CANFD_BRS" BO_ 929 0;'))
        self.assertTrue(result["frames"][2]["fd"])
        self.assertFalse(result["frames"][2]["brs"])

    def test_frame_format_defaults_and_invalid_brs_are_checked(self):
        text = DBC.replace('BA_ "VFrameFormat" BO_ 929 14;', 'BA_DEF_DEF_ "VFrameFormat" 14;')
        result = self.parse_dbc(text)
        self.assertTrue(result["frames"][2]["fd"])
        for bad in ('BA_ "CANFD_BRS" BO_ 929 2;', 'BA_ "CANFD_BRS" BO_ 291 1;'):
            with self.assertRaises(DatabaseImportError):
                self.parse_dbc(DBC.replace('BA_ "CANFD_BRS" BO_ 929 1;', bad))

    def test_explicit_brs_and_cycle_defaults_are_honored(self):
        text = DBC.replace('BA_ "GenMsgCycleTime" BO_ 929 20;', 'BA_DEF_DEF_ "GenMsgCycleTime" 40;')
        text = text.replace('BA_ "CANFD_BRS" BO_ 929 1;', 'BA_DEF_DEF_ "CANFD_BRS" 1;\nBA_ "CANFD_BRS" BO_ 291 0;\nBA_ "CANFD_BRS" BO_ 2566869221 0;')
        result = self.parse_dbc(text)
        self.assertTrue(result["frames"][2]["brs"])
        self.assertEqual(result["frames"][2]["cycle_ms"],40)

    def test_independent_signal_placeholder_is_explicitly_skipped(self):
        text = DBC + 'BO_ 3221225472 VECTOR__INDEPENDENT_SIG_MSG: 0 Vector__XXX\n SG_ FreeSignal : 0|8@1+ (1,0) [0|255] "" Vector__XXX\n'
        result = self.parse_dbc(text)
        self.assertEqual(len(result["frames"]),3)
        self.assertTrue(any("独立信号占位" in warning for warning in result["warnings"]))

    def test_real_busmaster_dbf_classic_and_extended_without_bit31(self):
        self.assertIn("ExtendedStatus,419385573,8,0,0,X", DBF)
        result = parse_database("review.dbf", DBF.encode())
        self.assertEqual(result["format_label"], "BUSMASTER DBF")
        self.assertEqual([(f["id"], f["extended"], f["length"]) for f in result["frames"]],
                         [(0x123, False, 8), (0x18FF50E5, True, 8)])
        self.assertEqual(result["frames"][0]["signals"][0]["name"], "Speed")
        self.assertEqual(result["frames"][0]["cycle_ms"], 100)
        self.assertTrue(any("100 ms" in warning for warning in result["warnings"]))

    def test_busmaster_cycle_attribute(self):
        text = DBF.replace('[START_PARAM_MSG]\n', '[START_PARAM_MSG]\n"GenMsgCycleTime",INT,0,65535,0\n')
        text = text.replace('[START_PARAM_MSG_VAL]\n', '[START_PARAM_MSG_VAL]\n291,S,"GenMsgCycleTime",250\n')
        # canmatrix DBF retains GenMsgCycleTime as an attribute; the adapter
        # explicitly reads it even though the library does not set cycle_time.
        result = parse_database("review.dbf", text.encode())
        self.assertEqual(result["frames"][0]["cycle_ms"], 250)

    def test_motorola_signal_startbit_is_the_source_convention(self):
        result = self.parse_dbc(DBC.replace('0|16@1+', '7|16@0+'))
        signal = result["frames"][0]["signals"][0]
        self.assertEqual((signal["start_bit"], signal["length"], signal["byte_order"]), (7,16,"big"))

    def test_utf8_bom_gb18030_and_cp1252(self):
        text = DBC.replace('"km/h"', '"千米/小时"')
        utf = parse_database("review.dbc", b"\xef\xbb\xbf" + text.encode())
        self.assertEqual(utf["frames"][0]["signals"][0]["unit"], "千米/小时")
        gb = parse_database("review.dbc", text.encode("gb18030"))
        self.assertEqual((gb["encoding"],gb["frames"][0]["signals"][0]["unit"]), ("gb18030","千米/小时"))
        latin = self.parse_dbc(DBC.replace('"km/h"','"café"'), "cp1252")
        self.assertEqual((latin["encoding"],latin["frames"][0]["signals"][0]["unit"]), ("cp1252","café"))
        explicit = parse_database("review.dbc", text.encode("gb18030"), "gbk")
        self.assertEqual(explicit["encoding"], "gb18030")
        with self.assertRaises(DatabaseImportError):
            parse_database("review.dbc", text.encode("gb18030"), "utf-8")

    def test_unrecognized_database_and_binary_dbf_are_rejected(self):
        for name, raw in (("capture.log",DBC.encode()), ("fake.dbf",DBC.encode()),
                          ("fake.dbc",DBF.encode()), ("table.dbf",b"\x03\x64\x01\x01\0\0"),
                          ("none.dbf",b"name,id\nDemo,123\n"), ("none.dbc",b"VERSION \"empty\"\n")):
            with self.subTest(name=name, raw=raw[:30]), self.assertRaises(DatabaseImportError):
                parse_database(name, raw)

    def test_database_size_frame_and_signal_bounds(self):
        for raw in (b"", b"x" * (database.MAX_DATABASE_BYTES + 1), b'VERSION "' + b"x" * 32769 + b'"\n'):
            with self.assertRaises(DatabaseImportError):
                parse_database("limit.dbc", raw)
        many = 'VERSION ""\n' + "\n".join(f"BO_ {0x80000000+i} Frame{i}: 8 Node" for i in range(4097))
        with self.assertRaisesRegex(DatabaseImportError,"4096"):
            self.parse_dbc(many)
        many_signals = 'BO_ 1 Test: 8 Node\n' + "\n".join(f'SG_ Sig{i} : 0|1@1+ (1,0) [0|1] "" Node' for i in range(257))
        with self.assertRaisesRegex(DatabaseImportError,"256"):
            self.parse_dbc(many_signals)

    def test_invalid_ids_are_not_silently_masked_by_canmatrix(self):
        for invalid_id in (2048, 0xA0000001, 0x180000001):
            text = f'BO_ {invalid_id} BadId: 8 Node\n'
            with self.subTest(invalid_id=invalid_id), self.assertRaises(DatabaseImportError):
                self.parse_dbc(text)
        for invalid_id in (-1, 0x180000001):
            text = DBF.replace('ExtendedStatus,419385573', f'ExtendedStatus,{invalid_id}')
            with self.assertRaises(DatabaseImportError):
                parse_database("bad.dbf", text.encode())

    def test_invalid_length_missing_fd_and_signal_outside_frame(self):
        for text in (DBC.replace('FdPayload: 64','FdPayload: 9'),
                     DBC.replace('BA_ "VFrameFormat" BO_ 929 14;', 'BA_ "VFrameFormat" BO_ 929 0;'),
                     DBC.replace('504|8@1+', '511|8@1+')):
            with self.assertRaises(DatabaseImportError):
                self.parse_dbc(text)

    def test_canmatrix_printed_parse_errors_cannot_be_silently_swallowed(self):
        text = DBC.replace(' SG_ Speed : 0|16@1+', ' SG_ Speed : broken')
        printed = io.StringIO()
        with redirect_stdout(printed), redirect_stderr(printed), self.assertRaisesRegex(DatabaseImportError,"解析发现错误"):
            self.parse_dbc(text)
        self.assertEqual(printed.getvalue(), "")

    def test_unknown_dbc_line_and_truncated_comment_rejected(self):
        for text in (DBC + "BROKEN RECOVERY FILE\n", DBC + 'CM_ BO_ 291 "unclosed\n'):
            with self.assertRaises(DatabaseImportError):
                self.parse_dbc(text)

    def test_busmaster_count_signal_and_end_markers_validated(self):
        for text in (DBF.replace('[NUMBER_OF_MESSAGES] 2', '[NUMBER_OF_MESSAGES] 3'),
                     DBF.replace('ClassicStatus,291,8,1','ClassicStatus,291,8,2'),
                     DBF.replace('[END_MSG]','',1),
                     DBF.replace('[PROTOCOL] CAN','[PROTOCOL] LIN'),
                     DBF.replace('ExtendedStatus,419385573,8','ExtendedStatus,419385573,12')):
            with self.assertRaises(DatabaseImportError):
                parse_database("bad.dbf", text.encode())

    def test_noncan_and_canxl_are_rejected(self):
        lin = DBC + 'BA_DEF_ "BusType" STRING;\nBA_ "BusType" "LIN";\n'
        xl = DBC.replace('"StandardCAN_FD"', '"StandardCANXL"')
        for text in (lin,xl):
            with self.assertRaises(DatabaseImportError):
                self.parse_dbc(text)

    def test_duplicate_ids_do_not_create_an_ambiguous_catalog(self):
        with self.assertRaisesRegex(DatabaseImportError,"重复"):
            self.parse_dbc(DBC + 'BO_ 291 AnotherName: 8 DemoNode\n')

    def test_missing_dependency_has_an_actionable_error(self):
        real_import = __import__
        def import_without_canmatrix(name, *args, **kwargs):
            if name == "canmatrix.formats":
                raise ImportError("test missing dependency")
            return real_import(name, *args, **kwargs)
        with mock.patch("builtins.__import__", side_effect=import_without_canmatrix), self.assertRaisesRegex(DatabaseImportError,"requirements.txt"):
            self.parse_dbc()


if __name__ == "__main__":
    unittest.main(verbosity=2)
