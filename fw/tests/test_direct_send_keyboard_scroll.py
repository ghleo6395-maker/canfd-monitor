"""Device keyboard and scrolling journeys through an isolated real HTTP server.

No request contacts the user's 8765/8766 services or persistent plan directory.
Configuration changes under test use the device's keyboard, not PC TX forms.
"""
from copy import deepcopy
import json
import tempfile
import unittest

import test_web as transport
from send_plan_format import PlanStore


class DirectSendKeyboardScrollTests(unittest.TestCase):
    setUpClass = classmethod(transport.WebTests.setUpClass.__func__)
    tearDownClass = classmethod(transport.WebTests.tearDownClass.__func__)
    request, post, state = transport.WebTests.request, transport.WebTests.post, transport.WebTests.state
    boot, advance, key = transport.WebTests.boot, transport.WebTests.advance, transport.WebTests.key

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="canfd-direct-keyboard-")
        self.addCleanup(self.directory.cleanup)
        self.previous_store = transport.web.plan_store
        transport.web.plan_store = PlanStore(self.directory.name)
        self.addCleanup(self.restore_store)
        transport.WebTests.setUp(self)

    def restore_store(self):
        with transport.web.lock:
            transport.web.plan_store = self.previous_store

    def generation(self):
        return self.state()["sim"]["generation"]

    def ui(self):
        return self.state()["dev"]["ui"]

    def navigate(self, page):
        self.post("/api/ui", action="navigate", page=page, generation=self.generation())
        self.assertEqual(self.ui()["page"], "tx" if page == "send" else page)

    def tap(self, label):
        ui = self.ui()
        rows = [index for index, item in enumerate(ui.get("items", [])) if item["l"] == label]
        self.assertEqual(len(rows), 1, (label, ui))
        payload = {"kind": 0, "a": rows[0], "generation": self.generation()}
        if "keyboard" in ui:
            payload["keyboard_token"] = ui["keyboard"]["token"]
        if ui["page"] == "send_files":
            file = next((file for file in self.state()["work"]["send_plans"]["files"]
                         if file["index"] == rows[0] and file["name"] == label), None)
            if file:
                payload.update(plan_token=file["token"], plan_name=file["name"])
        self.post("/api/touch", **payload)

    def input_text(self, value):
        ui = self.ui()
        self.assertEqual(ui["page"], "local_input")
        token = ui["keyboard"]["token"]
        self.tap("清空")
        keys = {key["value"]: key["index"] for key in self.ui()["keyboard"]["keys"]
                if key["kind"] == "text"}
        for character in value:
            self.assertIn(character, keys, (character, self.ui()["keyboard"]))
            self.post("/api/touch", kind=0, a=keys[character], keyboard_token=token,
                      generation=self.generation())
        self.assertEqual(self.ui()["keyboard"]["value"], value)
        self.assertEqual(self.ui()["keyboard"]["token"], token)

    def open_editor(self, channel=0, entry=0):
        self.navigate("send")
        self.post("/api/tx/edit", ch=channel, index=entry)
        self.assertEqual(self.ui()["page"], "txedit")

    def assert_no_tx(self):
        state = self.state()
        self.assertTrue(all(entry["sent"] == 0 and not entry["active"]
                            for channel in state["work"]["tx"] for entry in channel))
        self.assertTrue(all(channel["state"] == 0 for channel in state["work"]["sequence"]))
        self.assertFalse(state["dev"]["ui"]["dlg"].get("kind"))

    def scroll(self, rows):
        live = self.ui()["live"]
        return self.post("/api/ui", action="live_scroll", rows=rows, channel=live["channel"],
                         view=1 if live["view"] == "byid" else 0, generation=self.generation())

    def test_home_and_send_navigation_open_the_editable_list_directly(self):
        self.navigate("home")
        self.tap("发送报文")
        self.assertEqual(self.ui()["page"], "tx")
        self.assertIn("加载文件", [item["l"] for item in self.ui()["items"]])
        self.assertNotIn("现有报文", [item["l"] for item in self.ui()["items"]])
        self.navigate("home")
        self.navigate("send")
        self.assert_no_tx()

    def test_loading_can2_fd_file_inherits_full_format_without_an_extra_step(self):
        status, plan = self.request("/api/plan/template?example=single")
        self.assertEqual(status, 200, plan)
        self.post("/api/plan/import", name="Inherited.canplan.json", text=json.dumps(plan),
                  generation=self.generation())
        self.navigate("send")
        before = deepcopy(self.state()["work"]["tx"][0])
        self.tap("加载文件")
        self.assertEqual(self.ui()["page"], "send_files")
        self.tap("Inherited.canplan.json")
        state = self.state()
        self.assertEqual(state["dev"]["ui"]["page"], "tx")
        self.assertEqual((state["dev"]["ui"]["tx_channel"], state["dev"]["ui"]["tx_scope_mask"]), (1, 2))
        self.assertEqual(state["work"]["tx"][0], before)
        for slot, expected in enumerate(plan["channels"][0]["entries"]):
            actual = state["work"]["tx"][1][slot]
            flags = int(expected["extended"]) | (2 if expected["fd"] else 0) | (4 if expected["brs"] else 0) | (16 if expected["rtr"] else 0)
            self.assertEqual((actual["id"], actual["flags"], actual["len"], actual["data"]),
                             (int(expected["id"], 16), flags, expected["length"], expected["data"]))
            self.assertEqual(state["work"]["sequence"][1]["entries"][slot],
                             {key: expected[key] for key in ("enabled", "repeat_count", "frame_gap_ms", "cycle_ms")})
        self.assert_no_tx()

    def test_hex_id_keyboard_cancel_invalid_and_outer_save_are_distinct(self):
        self.open_editor()
        original = deepcopy(self.state()["work"]["tx"])
        self.tap("报文 ID")
        self.assertEqual(self.ui()["keyboard"]["mode"], "hex")
        self.input_text("321")
        self.tap("取消")
        self.assertEqual(self.ui()["page"], "txedit")
        self.assertEqual(self.ui()["editor"]["id"], original[0][0]["id"])
        self.tap("报文 ID")
        self.input_text("800")  # Standard frame ID cannot silently become extended.
        self.tap("确定")
        self.assertEqual(self.ui()["page"], "local_input")
        self.assertTrue(self.ui()["local_error"])
        self.assertEqual(self.state()["work"]["tx"], original)
        self.input_text("456")
        self.tap("确定")
        self.assertEqual(self.ui()["editor"]["id"], 0x456)
        self.assertEqual(self.state()["work"]["tx"], original)
        self.tap("保存")
        self.assertEqual(self.ui()["page"], "tx")
        actual = self.state()["work"]["tx"]
        self.assertEqual(actual[0][0]["id"], 0x456)
        self.assertEqual(actual[0][0]["flags"], original[0][0]["flags"])
        self.assertEqual(actual[0][1:], original[0][1:])
        self.assertEqual(actual[1], original[1])
        self.assert_no_tx()

    def test_full_64_byte_payload_keyboard_never_truncates_or_autosends(self):
        self.open_editor(0, 2)  # Existing FD/BRS slot is already 64 bytes.
        original = deepcopy(self.state()["work"]["tx"])
        original_data = deepcopy(self.ui()["editor"]["data"])
        self.tap("完整数据")
        self.assertEqual(self.ui()["keyboard"]["mode"], "payload")
        self.assertGreaterEqual(self.ui()["keyboard"]["max_bytes"], 128)
        self.input_text("AA BB")
        self.tap("取消")
        self.assertEqual(self.ui()["editor"]["data"], original_data)
        self.tap("完整数据")
        for invalid in ("ABC", "00"):
            self.input_text(invalid)
            self.tap("确定")
            self.assertEqual(self.ui()["page"], "local_input")
            self.assertTrue(self.ui()["local_error"])
            self.assertEqual(self.state()["work"]["tx"], original)
        payload = bytes((index * 7 + 3) & 255 for index in range(64))
        self.input_text(payload.hex().upper())
        self.tap("确定")
        self.assertEqual(self.ui()["page"], "txedit")
        self.assertEqual(self.ui()["editor"]["data"], list(payload))
        self.assertEqual(self.state()["work"]["tx"], original)
        self.tap("完整数据")
        self.input_text(" ".join(f"{byte:02X}" for byte in payload))
        self.assertEqual(len(self.ui()["keyboard"]["value"]), 191)
        self.tap("确定")
        self.assertEqual(self.ui()["editor"]["data"], list(payload))
        self.tap("保存")
        saved = self.state()["work"]["tx"][0][2]
        self.assertEqual((saved["len"], saved["flags"]), (64, original[0][2]["flags"]))
        self.assertEqual(bytes.fromhex(saved["data"]), payload)
        self.assertEqual(len(bytes.fromhex(saved["data"])), 64)
        self.assert_no_tx()

    def test_decimal_count_and_cycle_keyboard_rejects_cross_field_conflicts(self):
        self.navigate("send")
        original = deepcopy(self.state()["work"]["sequence"])
        self.post("/api/local", kind=1, ch=1, index=0, generation=self.generation())
        self.tap("每轮次数")
        self.assertEqual(self.ui()["keyboard"]["mode"], "decimal")
        self.input_text("0")
        self.tap("确定")
        self.assertEqual(self.ui()["page"], "local_input")
        self.assertTrue(self.ui()["local_error"])
        self.input_text("10")
        self.tap("确定")
        self.assertEqual(self.ui()["page"], "txtiming")
        self.assertEqual(self.state()["work"]["sequence"], original)
        self.tap("同 ID 间隔")
        self.input_text("7")
        self.tap("确定")
        self.tap("单 ID 总周期")
        self.input_text("63")  # Must exceed (10 - 1) * 7.
        self.tap("确定")
        self.assertEqual(self.ui()["page"], "local_input")
        self.assertTrue(self.ui()["local_error"])
        self.input_text("120")
        self.tap("确定")
        self.assertEqual(self.state()["work"]["sequence"], original)
        self.tap("保存本路设置")
        actual = self.state()["work"]["sequence"]
        self.assertEqual(actual[0], original[0])
        self.assertEqual(actual[1]["entries"][0],
                         {"enabled": True, "repeat_count": 10, "frame_gap_ms": 7, "cycle_ms": 120})
        self.assertEqual(actual[1]["entries"][1:], original[1]["entries"][1:])
        self.assert_no_tx()

    def test_fast_keyboard_input_and_stale_tokens_never_target_a_new_draft(self):
        self.open_editor()
        original = deepcopy(self.state()["work"]["tx"])
        self.tap("报文 ID")
        old_token = self.ui()["keyboard"]["token"]
        generation = self.generation()
        self.post("/api/local/input", action="replace", text="", token=old_token,
                  generation=generation)
        for digit in "321":
            self.post("/api/local/input", action="append", text=digit, token=old_token,
                      generation=generation)
        self.assertEqual(self.ui()["keyboard"]["value"], "321")
        self.assertEqual(self.state()["work"]["tx"], original)
        status, _ = self.request("/api/local/input", {"action": "append", "text": "G",
                                                     "token": old_token, "generation": generation})
        self.assertEqual(status, 409)
        self.assertEqual(self.ui()["keyboard"]["value"], "321")
        self.tap("取消")
        self.tap("报文 ID")
        fresh = deepcopy(self.ui()["keyboard"])
        self.assertNotEqual(fresh["token"], old_token)
        for action in ("append", "replace"):
            status, _ = self.request("/api/local/input", {"action": action, "text": "1",
                                                         "token": old_token, "generation": generation})
            self.assertEqual(status, 409)
            self.assertEqual(self.ui()["keyboard"], fresh)
        status, _ = self.request("/api/local/input", {"action": "replace", "text": "1",
                                                     "token": fresh["token"], "generation": generation - 1})
        self.assertEqual(status, 409)
        self.assertEqual(self.ui()["keyboard"], fresh)
        status, _ = self.request("/api/local/input", {"action": "append", "text": "1",
                                                     "token": fresh["token"]})
        self.assertEqual(status, 400)
        self.assertEqual(self.ui()["keyboard"], fresh)
        self.assertEqual(self.state()["work"]["tx"], original)
        self.assert_no_tx()

    def start_background_recording_and_tx(self):
        self.navigate("send")
        state = self.state()
        self.post("/api/sequence", action="configure", ch=0,
                  entries=[{"enabled": index == 0, "repeat_count": 1,
                            "frame_gap_ms": 5, "cycle_ms": 20} for index in range(4)],
                  id_gap_ms=3, revision=state["work"]["sequence"][0]["revision"],
                  generation=self.generation())
        self.post("/api/tx/scope/control", action="start", mask=1, index=0, generation=self.generation())
        self.assertTrue(self.ui()["dlg"].get("kind"))
        self.post("/api/touch", kind=2, a=0, generation=self.generation())
        self.advance(30)
        self.navigate("live")
        self.post("/api/ui", action="live_channel", channel=2, generation=self.generation())
        self.post("/api/record", action="start", generation=self.generation())
        self.advance(300)
        state = self.state()
        self.assertEqual(state["dev"]["rec"]["sid"], 2)
        self.assertEqual(state["work"]["sequence"][0]["state"], 3)
        self.assertGreater(state["dev"]["ui"]["live"]["total"], 12)

    def test_history_scroll_preserves_channel_and_background_recording_and_tx(self):
        self.start_background_recording_and_tx()
        before = deepcopy(self.state())
        self.scroll(5)
        frozen = deepcopy(self.ui()["live"])
        self.assertEqual((frozen["channel"], frozen["cur"], frozen["paused"]), (2, 5, True))
        self.assertEqual(frozen["filter_cfg"], before["dev"]["ui"]["live"]["filter_cfg"])
        self.assertTrue(all(row["ch"] == 2 for row in frozen["rows"]))
        self.advance(200)
        after = self.state()
        self.assertEqual(after["dev"]["rec"]["session"], before["dev"]["rec"]["session"])
        self.assertGreater(after["dev"]["rec"]["frames"], before["dev"]["rec"]["frames"])
        self.assertGreater(after["work"]["sequence"][0]["sent"], before["work"]["sequence"][0]["sent"])
        self.assertEqual(after["dev"]["ui"]["live"]["rows"], frozen["rows"])
        self.assertEqual(after["dev"]["ui"]["live"]["view_time_us"], frozen["view_time_us"])
        self.scroll(-5)
        self.assertEqual((self.ui()["live"]["cur"], self.ui()["live"]["paused"]), (0, True))
        stationary = deepcopy(self.ui()["live"])
        self.scroll(0)
        self.assertEqual(self.ui()["live"], stationary)
        total = self.ui()["live"]["total"]
        for _ in range(total // 128 + 2):
            self.scroll(128)
        self.assertEqual(self.ui()["live"]["cur"], total - 1)
        for _ in range(total // 128 + 2):
            self.scroll(-128)
        self.assertEqual((self.ui()["live"]["cur"], self.ui()["live"]["paused"]), (0, True))
        self.post("/api/ui", action="live_pause", paused=False, generation=self.generation())
        self.assertEqual((self.ui()["live"]["cur"], self.ui()["live"]["paused"]), (0, False))

    def test_id_summary_scroll_keeps_pause_and_channel_cursor_memory(self):
        self.navigate("live")
        self.advance(300)
        self.post("/api/ui", action="live_view", view=1, generation=self.generation())
        first_cursor = min(2, self.ui()["live"]["total"] - 1)
        self.assertGreater(first_cursor, 0)
        self.scroll(2)
        self.assertEqual((self.ui()["live"]["cur"], self.ui()["live"]["paused"]), (first_cursor, False))
        self.post("/api/ui", action="live_channel", channel=2, generation=self.generation())
        second_cursor = min(1, self.ui()["live"]["total"] - 1)
        self.assertGreaterEqual(second_cursor, 0)
        self.scroll(1)
        self.assertEqual((self.ui()["live"]["channel"], self.ui()["live"]["cur"]), (2, second_cursor))
        self.post("/api/ui", action="live_channel", channel=1, generation=self.generation())
        self.assertEqual(self.ui()["live"]["cur"], first_cursor)
        self.post("/api/ui", action="live_pause", paused=True, generation=self.generation())
        self.scroll(128)
        self.assertEqual(self.ui()["live"]["cur"], self.ui()["live"]["total"] - 1)
        self.assertTrue(self.ui()["live"]["paused"])
        self.scroll(-128)
        self.assertEqual((self.ui()["live"]["cur"], self.ui()["live"]["paused"]), (0, True))

    def test_scroll_invalid_empty_and_non_live_requests_are_atomic(self):
        self.navigate("live")
        self.post("/api/filter", channel=0, type=0, id="7FF", mask="7FF", direction=0, format=0)
        self.assertEqual(self.ui()["live"]["total"], 0)
        self.scroll(128)
        self.assertEqual((self.ui()["live"]["cur"], self.ui()["live"]["paused"]), (0, False))
        before = deepcopy(self.state())
        for invalid in (-129, 129, True, 1.5, "1", None):
            status, _ = self.request("/api/ui", {"action": "live_scroll", "rows": invalid,
                                                "generation": self.generation()})
            self.assertEqual(status, 400)
            self.assertEqual(self.ui()["live"], before["dev"]["ui"]["live"])
            self.assertEqual(self.state()["work"], before["work"])
        for stale_context in ({"channel": 2, "view": 0}, {"channel": 1, "view": 1}):
            status, _ = self.request("/api/ui", {"action": "live_scroll", "rows": 1,
                                                "generation": self.generation(), **stale_context})
            self.assertEqual(status, 409)
            self.assertEqual(self.ui()["live"], before["dev"]["ui"]["live"])
        status, _ = self.request("/api/ui", {"action": "live_scroll", "rows": 1,
                                            "generation": self.generation() + 1})
        self.assertEqual(status, 409)
        self.assertEqual(self.ui()["live"], before["dev"]["ui"]["live"])
        self.open_editor()
        self.tap("报文 ID")
        keyboard = deepcopy(self.ui()["keyboard"])
        for context in ({}, {"channel": 1, "view": 0}):
            status, _ = self.request("/api/ui", {"action": "live_scroll", "rows": 1,
                                                "generation": self.generation(), **context})
            self.assertEqual(status, 409)
            self.assertEqual(self.ui()["page"], "local_input")
            self.assertEqual(self.ui()["keyboard"], keyboard)
        self.assert_no_tx()
        self.tap("取消")
        self.tap("取消")
        self.navigate("live")
        self.post("/api/record", action="start", generation=self.generation())
        self.advance(200)
        self.post("/api/record", action="stop", generation=self.generation())
        status, _ = self.request("/api/ui", {"action": "live_scroll", "rows": 1,
                                            "generation": self.generation()})
        self.assertEqual(status, 409)  # Pending close also owns a local draft.
        self.advance(300)
        self.assertEqual(self.ui()["page"], "local_record_save")
        recording_keyboard = deepcopy(self.ui()["keyboard"])
        status, _ = self.request("/api/ui", {"action": "live_scroll", "rows": 1,
                                            "generation": self.generation()})
        self.assertEqual(status, 409)
        self.assertEqual(self.ui()["page"], "local_record_save")
        self.assertEqual(self.ui()["keyboard"], recording_keyboard)
        self.tap("保留默认名")


if __name__ == "__main__":
    unittest.main(verbosity=2)
