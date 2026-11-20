"""Real HTTP regressions for the device menu, recording and send workflows.

Use a random-port server, a manually stepped clock and an isolated plan store;
the user's simulator and on-disk plans are never part of these tests.
"""
from copy import deepcopy
import json
from pathlib import Path
import tempfile
import unittest

import test_web as transport
from send_plan_format import PlanStore


class MenuWorkflowTests(unittest.TestCase):
    setUpClass = classmethod(transport.WebTests.setUpClass.__func__)
    tearDownClass = classmethod(transport.WebTests.tearDownClass.__func__)
    request, post, state = transport.WebTests.request, transport.WebTests.post, transport.WebTests.state
    boot, advance, key = transport.WebTests.boot, transport.WebTests.advance, transport.WebTests.key
    navigate = transport.WebTests.navigate

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="canfd-menu-http-")
        self.addCleanup(self.directory.cleanup)
        self.previous_store = transport.web.plan_store
        transport.web.plan_store = PlanStore(self.directory.name)
        self.addCleanup(self.restore_store)
        transport.WebTests.setUp(self)

    def restore_store(self):
        with transport.web.lock:
            transport.web.plan_store = self.previous_store

    def ui(self):
        return self.state()["dev"]["ui"]

    def item_index(self, label):
        items = self.ui().get("items", [])
        matches = [index for index, item in enumerate(items) if item.get("l") == label]
        self.assertEqual(len(matches), 1, (label, items))
        return matches[0]

    def tap(self, label):
        self.post("/api/touch", kind=0, a=self.item_index(label))

    def record(self, action):
        return self.post("/api/record", action=action, generation=self.state()["sim"]["generation"])

    def scope(self, mask):
        return self.post("/api/tx/scope", mask=mask, generation=self.state()["sim"]["generation"])

    def scope_control(self, action, mask, index=0):
        return self.post("/api/tx/scope/control", action=action, mask=mask, index=index,
                         generation=self.state()["sim"]["generation"])

    def confirm_pending(self):
        for _ in range(4):
            if not self.ui()["dlg"].get("kind"):
                break
            self.post("/api/touch", kind=2, a=0)
            self.advance(20)
        self.assertFalse(self.ui()["dlg"].get("kind"))

    def configure_groups(self):
        for channel in range(2):
            self.post("/api/env", cmd="bus_fps", a=channel, b=0)
            self.post("/api/sequence", action="configure", ch=channel,
                      entries=[{"enabled": index == 0, "repeat_count": 2,
                                "frame_gap_ms": 5, "cycle_ms": 20} for index in range(4)],
                      id_gap_ms=3, revision=self.state()["work"]["sequence"][channel]["revision"],
                      generation=self.state()["sim"]["generation"])

    def import_project(self, name, project, template="single"):
        status, plan = self.request("/api/plan/template?example=" + template)
        self.assertEqual(status, 200, plan)
        self.post("/api/plan/import", name=name, text=json.dumps(plan), project=project,
                  generation=self.state()["sim"]["generation"])
        return next(file for file in self.state()["work"]["send_plans"]["files"] if file["name"] == name)

    def start_live_recording(self):
        self.navigate("live")
        self.record("start")
        self.advance(200)
        self.assertEqual(self.state()["dev"]["rec"]["sid"], 2)
        self.assertEqual(self.ui()["page"], "live")

    def test_display_pause_and_fixed_detail_keep_live_recording_running(self):
        self.start_live_recording()
        self.post("/api/ui", action="live_pause", paused=True)
        frozen = deepcopy(self.ui()["live"])
        before = self.state()["dev"]["rec"]
        self.advance(200)
        after = self.state()["dev"]["rec"]
        self.assertGreater(after["frames"], before["frames"])
        self.assertEqual(self.ui()["live"]["rows"], frozen["rows"])
        self.assertEqual(self.ui()["live"]["view_time_us"], frozen["view_time_us"])
        self.assertGreater(self.ui()["live"]["hidden"], frozen["hidden"])
        self.post("/api/ui", action="live_detail", seq=frozen["rows"][0]["seq"])
        detail = deepcopy(self.ui()["detail"])
        before = self.state()["dev"]["rec"]["frames"]
        self.advance(200)
        self.assertEqual(self.ui()["page"], "detail")
        self.assertEqual(self.ui()["detail"], detail)
        self.assertGreater(self.state()["dev"]["rec"]["frames"], before)
        self.key(5)
        returned = self.ui()["live"]
        self.assertTrue(returned["paused"])
        self.assertEqual(returned["view"], frozen["view"])
        self.assertEqual(returned["cur"], frozen["cur"])

    def test_live_mark_stop_and_next_session_preserve_paused_display(self):
        self.start_live_recording()
        self.post("/api/ui", action="live_pause", paused=True)
        first = deepcopy(self.state()["work"]["files"][-1])
        frozen = deepcopy(self.ui()["live"]["rows"])
        markers = self.state()["dev"]["rec"]["markers"]
        self.record("mark")
        self.assertEqual(self.state()["dev"]["rec"]["markers"], markers + 1)
        self.record("stop")
        self.advance(300)
        self.assertEqual(self.ui()["page"], "local_record_save")
        transport.finish_record_save(self, "保存")
        stopped = self.state()
        self.assertEqual(stopped["dev"]["rec"]["sid"], 0)
        self.assertEqual(stopped["dev"]["ui"]["page"], "live")
        self.assertTrue(stopped["dev"]["ui"]["live"]["paused"])
        self.assertEqual(stopped["dev"]["ui"]["live"]["rows"], frozen)
        self.assertFalse(stopped["work"]["files"][-1]["writing"])
        generation = stopped["sim"]["generation"]
        self.record("start")
        self.advance(200)
        restarted = self.state()
        self.assertEqual(restarted["sim"]["generation"], generation)
        self.assertEqual(restarted["dev"]["rec"]["sid"], 2)
        self.assertEqual(restarted["dev"]["ui"]["page"], "live")
        self.assertNotEqual(restarted["work"]["files"][-1]["token"], first["token"])
        self.assertNotEqual(restarted["work"]["files"][-1]["name"], first["name"])

    def test_recording_settings_return_to_original_paused_live_context(self):
        self.start_live_recording()
        self.post("/api/ui", action="live_view", view=1)
        self.post("/api/ui", action="live_pause", paused=True)
        original = deepcopy(self.ui()["live"])
        before = self.state()["dev"]["rec"]["frames"]
        self.navigate("rec")
        self.advance(100)
        self.key(5)
        self.assertEqual(self.ui()["page"], "live")
        returned = self.ui()["live"]
        self.assertEqual((returned["view"], returned["paused"], returned["cur"]),
                         (original["view"], original["paused"], original["cur"]))
        self.assertEqual(returned["rows"], original["rows"])
        self.assertGreater(self.state()["dev"]["rec"]["frames"], before)

    def test_file_storage_actions_reject_running_and_closing_recordings(self):
        self.start_live_recording()
        self.post("/api/env", cmd="usb", a=1)
        self.navigate("files")
        files = deepcopy(self.state()["work"]["files"])
        for label in ("进入 U 盘模式", "格式化 SD 卡"):
            self.tap(label)
            state = self.state()
            self.assertEqual(state["dev"]["storage"]["state"], "就绪")
            self.assertEqual(state["dev"]["rec"]["sid"], 2)
            self.assertFalse(state["dev"]["ui"]["dlg"].get("kind"))
            self.assertEqual(state["work"]["files"], files)
        self.record("stop")
        self.assertEqual(self.state()["dev"]["rec"]["sid"], 3)
        for label in ("进入 U 盘模式", "格式化 SD 卡"):
            self.tap(label)
            state = self.state()
            self.assertEqual(state["dev"]["storage"]["state"], "就绪")
            self.assertFalse(state["dev"]["ui"]["dlg"].get("kind"))
        self.advance(300)
        self.assertEqual(self.state()["dev"]["rec"]["sid"], 0)
        transport.finish_record_save(self, "保留默认名")
        closed_files = deepcopy(self.state()["work"]["files"])
        self.tap("格式化 SD 卡")
        self.assertTrue(self.ui()["dlg"].get("kind"))
        self.post("/api/touch", kind=2, a=1)
        self.assertEqual(self.state()["work"]["files"], closed_files)
        self.assertEqual(self.state()["dev"]["storage"]["state"], "就绪")

    def test_send_existing_files_are_flat_and_load_does_not_send(self):
        self.import_project("A.canplan.json", "台架A")
        self.import_project("B.canplan.json", "路试B", "dual")
        self.navigate("send")
        self.assertEqual(self.ui()["page"], "tx")
        self.tap("加载文件")
        self.assertEqual(self.ui()["page"], "send_files")
        names = [item["l"] for item in self.ui()["items"]]
        self.assertIn("A.canplan.json", names)
        self.assertIn("B.canplan.json", names)
        self.assertNotIn("台架A", names)
        self.assertNotIn("路试B", names)
        self.tap("A.canplan.json")
        self.assertEqual(self.ui()["page"], "tx")
        self.assertEqual(self.ui()["tx_channel"], 1)
        self.assertEqual(self.ui()["tx_scope_mask"], 2)
        self.assertEqual(self.state()["work"]["tx"][1][0]["data"], "00 01 02 03 04 05 06 07")
        self.assertIn("加载文件", [item["l"] for item in self.ui()["items"]])
        self.assertTrue(all(not entry["active"] and entry["sent"] == 0
                            for channel in self.state()["work"]["tx"] for entry in channel))
        self.assertTrue(all(channel["state"] == 0 for channel in self.state()["work"]["sequence"]))
        self.assertFalse(self.ui()["dlg"].get("kind"))
        self.post("/api/sim", reset=True, pause=True)
        self.boot()
        self.navigate("send_files")
        self.assertIn("B.canplan.json", [item["l"] for item in self.ui()["items"]])

    def test_manual_file_picker_cancel_preserves_both_lists_scope_and_channel(self):
        self.import_project("Cancel.canplan.json", "取消加载")
        self.navigate("tx")
        self.post("/api/tx/select", ch=1, index=2, generation=self.state()["sim"]["generation"])
        self.scope(3)
        before = deepcopy(self.state()["work"])
        self.tap("加载文件")
        self.assertEqual(self.ui()["page"], "send_files")
        self.key(5)
        self.assertEqual(self.ui()["page"], "tx")
        self.assertEqual((self.ui()["tx_channel"], self.ui()["tx_scope_mask"]), (1, 3))
        self.assertEqual(self.state()["work"]["tx"], before["tx"])
        self.assertEqual(self.state()["work"]["sequence"], before["sequence"])
        self.navigate("send")
        self.tap("加载文件")
        self.key(5)
        self.assertEqual(self.ui()["page"], "tx")

    def test_manual_file_load_replaces_dual_lists_and_remains_editable(self):
        status, plan = self.request("/api/plan/template?example=dual")
        self.assertEqual(status, 200, plan)
        plan["channels"][0]["entries"][0].update(id="345", data="10 20 30 40 50 60 70 80")
        plan["channels"][1]["entries"][0].update(id="456", data="88 77 66 55 44 33 22 11")
        self.post("/api/plan/import", name="New.canplan.json", text=json.dumps(plan),
                  generation=self.state()["sim"]["generation"])
        self.navigate("tx")
        self.post("/api/tx/select", ch=1, index=0, generation=self.state()["sim"]["generation"])
        self.tap("加载文件")
        self.tap("New.canplan.json")
        self.assertEqual(self.ui()["page"], "tx")
        self.assertEqual((self.ui()["tx_channel"], self.ui()["tx_scope_mask"]), (1, 3))
        for channel in plan["channels"]:
            ch = channel["channel"] - 1
            actual = self.state()["work"]
            self.assertEqual(actual["sequence"][ch]["id_gap_ms"], channel["id_gap_ms"])
            for index, expected in enumerate(channel["entries"]):
                entry, timing = actual["tx"][ch][index], actual["sequence"][ch]["entries"][index]
                self.assertEqual((entry["id"], entry["data"], entry["len"]),
                                 (int(expected["id"], 16), expected["data"], expected["length"]))
                self.assertEqual(timing, {key: expected[key] for key in
                                         ("enabled", "repeat_count", "frame_gap_ms", "cycle_ms")})
        self.assertFalse(self.state()["dev"]["tx_active"])
        self.post("/api/tx/edit", ch=1, index=0)
        self.assertEqual(self.ui()["page"], "txedit")
        self.tap("取消")
        self.assertEqual(self.ui()["page"], "tx")
        self.assertEqual(self.state()["work"]["tx"][1][0]["data"], "88 77 66 55 44 33 22 11")

    def test_loading_a_file_cannot_replace_a_running_channel(self):
        self.import_project("Busy.canplan.json", "发送中")
        self.configure_groups()
        self.navigate("tx")
        self.scope(2)
        self.scope_control("start", 2)
        self.confirm_pending()
        self.advance(100)
        self.tap("加载文件")
        before = deepcopy(self.state()["work"])
        self.tap("Busy.canplan.json")
        self.assertEqual(self.ui()["page"], "send_files")
        self.assertEqual(self.state()["work"]["tx"], before["tx"])
        self.assertEqual(self.state()["work"]["sequence"], before["sequence"])
        self.assertEqual(self.ui()["tx_scope_mask"], 2)
        self.key(5)
        self.assertEqual(self.ui()["page"], "tx")
        self.scope_control("stop", 2)

    def test_loading_can2_preserves_can1_background_transmission(self):
        self.import_project("CAN2.canplan.json", "独立通道")
        self.configure_groups()
        self.navigate("tx")
        self.scope(1)
        self.scope_control("start", 1)
        self.confirm_pending()
        self.advance(100)
        before = deepcopy(self.state()["work"])
        self.tap("加载文件")
        self.tap("CAN2.canplan.json")
        self.assertEqual(self.ui()["page"], "tx")
        self.assertEqual((self.ui()["tx_channel"], self.ui()["tx_scope_mask"]), (1, 2))
        self.assertEqual(self.state()["work"]["tx"][0], before["tx"][0])
        self.assertEqual(self.state()["work"]["sequence"][0], before["sequence"][0])
        self.advance(100)
        self.assertGreater(self.state()["work"]["sequence"][0]["sent"], before["sequence"][0]["sent"])
        self.assertTrue(all(entry["sent"] == 0 for entry in self.state()["work"]["tx"][1]))
        self.scope_control("stop", 1)

    def test_stale_file_row_is_rejected_before_loading_a_different_file(self):
        old = self.import_project("B.canplan.json", "旧目录")
        self.navigate("tx")
        self.tap("加载文件")
        click = {"kind": 0, "a": old["index"], "plan_token": old["token"],
                 "plan_name": old["name"], "generation": self.state()["sim"]["generation"]}
        self.import_project("A.canplan.json", "新目录", "dual")
        before = deepcopy(self.state()["work"])
        for replacement in ({}, {"generation": click["generation"] - 1},
                            {"plan_name": "wrong.canplan.json"}):
            payload = {**click, **replacement}
            status, result = self.request("/api/touch", payload)
            self.assertEqual(status, 409, result)
            self.assertEqual(self.ui()["page"], "send_files")
            self.assertEqual(self.state()["work"]["tx"], before["tx"])
            self.assertEqual(self.state()["work"]["sequence"], before["sequence"])
        current = next(file for file in self.state()["work"]["send_plans"]["files"]
                       if file["name"] == old["name"])
        valid = {**click, "a": current["index"], "plan_token": current["token"]}
        status, result = self.request("/api/touch", {key: value for key, value in valid.items()
                                                   if key != "generation"})
        self.assertEqual(status, 400, result)
        self.post("/api/touch", **valid)
        self.assertEqual(self.ui()["page"], "tx")
        self.assertEqual(self.ui()["tx_scope_mask"], 2)

    def test_files_show_actual_files_first_and_destructive_actions_last(self):
        file = self.import_project("Plan.canplan.json", "文件测试")
        self.navigate("files")
        labels = [item["l"] for item in self.ui()["items"]]
        logs = [entry["name"] for entry in self.state()["work"]["files"]]
        self.assertEqual(labels[:len(logs)], logs)
        self.assertEqual(labels[len(logs)], file["name"])
        self.assertEqual(labels[-2:], ["进入 U 盘模式", "格式化 SD 卡"])
        self.tap(logs[0])
        self.assertEqual(self.ui()["page"], "file_detail")
        self.key(5)
        self.assertEqual(self.ui()["page"], "files")
        self.tap(file["name"])
        self.assertEqual(self.ui()["page"], "plan_detail")
        self.assertTrue(all(entry["sent"] == 0 for channel in self.state()["work"]["tx"] for entry in channel))

    def test_manual_both_scope_is_independent_of_edit_channel_and_edits_are_isolated(self):
        self.navigate("send")
        self.assertEqual(self.ui()["page"], "tx")
        self.scope(3)
        before = deepcopy(self.state()["work"]["tx"])
        self.post("/api/tx/select", ch=1, index=2, generation=self.state()["sim"]["generation"])
        self.assertEqual(self.ui()["tx_scope_mask"], 3)
        self.assertEqual(self.ui()["tx_channel"], 1)
        self.post("/api/tx-entry", ch=1, index=2, id="321", flags=0, len=2,
                  data="AB CD", period=0, revision=before[1][2]["revision"])
        after = self.state()["work"]["tx"]
        self.assertEqual(after[0], before[0])
        self.assertEqual(after[1][:2], before[1][:2])
        self.assertEqual(after[1][3], before[1][3])
        self.assertEqual((after[1][2]["id"], after[1][2]["data"]), (0x321, "AB CD"))
        self.assertEqual(self.ui()["tx_scope_mask"], 3)
        self.assertTrue(all(entry["sent"] == 0 for channel in after for entry in channel))

    def test_dual_single_confirmation_cancel_sends_on_neither_channel(self):
        for channel in range(2):
            self.post("/api/env", cmd="bus_fps", a=channel, b=0)
        self.navigate("tx")
        self.scope(3)
        self.scope_control("single", 3)
        self.assertTrue(self.ui()["dlg"].get("kind"))
        self.assertTrue(all(entry["sent"] == 0 for channel in self.state()["work"]["tx"] for entry in channel))
        self.post("/api/touch", kind=2, a=1)
        self.advance(100)
        self.assertTrue(all(entry["sent"] == 0 for channel in self.state()["work"]["tx"] for entry in channel))
        self.assertEqual([channel["state"] for channel in self.state()["dev"]["ch"]], ["只听", "只听"])
        self.scope_control("single", 3)
        self.confirm_pending()
        self.advance(100)
        for channel in self.state()["work"]["tx"]:
            self.assertEqual(channel[0]["sent"], 1)
            self.assertTrue(all(entry["sent"] == 0 for entry in channel[1:]))

    def test_dual_loop_can_stop_one_channel_without_disturbing_the_other(self):
        self.configure_groups()
        self.navigate("tx")
        self.scope(3)
        self.scope_control("start", 3)
        self.assertTrue(self.ui()["dlg"].get("kind"))
        self.confirm_pending()
        self.advance(100)
        before = self.state()["work"]
        self.assertTrue(all(channel["state"] == 3 and channel["sent"] > 0 for channel in before["sequence"]))
        self.scope(1)
        self.scope_control("stop", 1)
        stopped = self.state()["work"]["tx"][0]
        self.advance(100)
        after = self.state()["work"]
        self.assertEqual(after["sequence"][0]["state"], 0)
        self.assertEqual(after["tx"][0], stopped)
        self.assertEqual(after["sequence"][1]["state"], 3)
        self.assertGreater(after["sequence"][1]["sent"], before["sequence"][1]["sent"])

    def test_dual_start_rejects_atomically_when_can2_is_already_running(self):
        self.configure_groups()
        self.navigate("tx")
        self.scope(2)
        self.scope_control("start", 2)
        self.confirm_pending()
        self.advance(60)
        self.scope(3)
        before = deepcopy(self.state()["work"])
        status, result = self.request("/api/tx/scope/control", {
            "action": "start", "mask": 3, "index": 0, "generation": self.state()["sim"]["generation"]})
        self.assertEqual(status, 409, result)
        self.assertEqual(self.state()["work"]["tx"], before["tx"])
        self.assertEqual(self.state()["work"]["sequence"], before["sequence"])
        self.assertFalse(self.ui()["dlg"].get("kind"))
        self.advance(60)
        self.assertTrue(all(entry["sent"] == 0 for entry in self.state()["work"]["tx"][0]))
        self.assertGreater(self.state()["work"]["sequence"][1]["sent"], before["sequence"][1]["sent"])

    def test_scope_validation_generation_and_local_draft_guards_are_atomic(self):
        self.navigate("tx")
        self.scope(3)
        for mask in (0, 4, True):
            before = deepcopy(self.state())
            for path, payload in (("/api/tx/scope", {"mask": mask}),
                                  ("/api/tx/scope/control", {"mask": mask, "action": "single", "index": 0})):
                status, result = self.request(path, {**payload, "generation": before["sim"]["generation"]})
                self.assertEqual(status, 400, result)
                self.assertEqual(self.state()["work"], before["work"])
                self.assertEqual(self.ui(), before["dev"]["ui"])
        self.post("/api/tx/edit", ch=0, index=0)
        before = deepcopy(self.state())
        status, result = self.request("/api/tx/scope/control", {
            "action": "single", "mask": 3, "index": 0, "generation": before["sim"]["generation"]})
        self.assertEqual(status, 409, result)
        self.assertEqual(self.state()["work"], before["work"])
        self.assertEqual(self.ui(), before["dev"]["ui"])
        generation = before["sim"]["generation"]
        self.post("/api/sim", reset=True, pause=True)
        self.boot()
        self.navigate("tx")
        for path, payload in (("/api/tx/scope", {"mask": 3}),
                              ("/api/tx/scope/control", {"mask": 3, "action": "single", "index": 0})):
            status, result = self.request(path, {**payload, "generation": generation})
            self.assertEqual(status, 409, result)
        self.assertEqual(self.ui()["tx_scope_mask"], 1)

    def test_unified_target_selection_syncs_channel_without_sending_or_stopping_background(self):
        self.navigate("tx")
        before = deepcopy(self.state())
        generation = before["sim"]["generation"]
        for mask, channel in ((2, 1), (1, 0), (2, 1)):
            self.post("/api/tx/target", mask=mask, generation=generation)
            current = self.state()
            self.assertEqual((self.ui()["tx_scope_mask"], self.ui()["tx_channel"]), (mask, channel))
            self.assertEqual(self.ui()["cursor"], channel * 4)
            self.assertEqual(current["work"]["tx"], before["work"]["tx"])
            self.assertEqual(current["work"]["sequence"], before["work"]["sequence"])
            self.assertEqual(current["dev"]["ch"], before["dev"]["ch"])
            self.assertFalse(self.ui()["dlg"].get("kind"))
        self.configure_groups()
        self.post("/api/tx/target", mask=1, generation=generation)
        self.scope_control("start", 1)
        self.confirm_pending()
        self.advance(60)
        running = deepcopy(self.state()["work"])
        self.assertEqual(running["sequence"][0]["state"], 3)
        for mask in (2, 3, 2):
            self.post("/api/tx/target", mask=mask, generation=generation)
            self.assertEqual(self.state()["work"]["tx"], running["tx"])
            self.assertEqual(self.state()["work"]["sequence"], running["sequence"])
        self.advance(60)
        after = self.state()["work"]
        self.assertGreater(after["sequence"][0]["sent"], running["sequence"][0]["sent"])
        self.assertEqual(after["sequence"][0]["revision"], running["sequence"][0]["revision"])
        self.assertEqual(after["sequence"][1]["state"], 0)
        self.assertTrue(all(entry["sent"] == 0 for entry in after["tx"][1]))
        self.scope_control("stop", 1)

    def test_unified_both_target_keeps_row_channel_single_and_edit_actions_independent(self):
        self.navigate("tx")
        generation = self.state()["sim"]["generation"]
        self.post("/api/tx/target", mask=2, generation=generation)
        self.post("/api/tx/target", mask=3, generation=generation)
        self.assertEqual((self.ui()["tx_scope_mask"], self.ui()["tx_channel"]), (3, 1))
        before = deepcopy(self.state()["work"]["tx"])
        for channel in (0, 1):
            # Same contract as a row button's immutable data-ch/data-index.
            self.post("/api/tx/control", action="single", ch=channel, index=0,
                      generation=generation)
            self.confirm_pending()
            self.advance(20)
            current = self.state()["work"]["tx"]
            for actual_channel in (0, 1):
                expected = before[actual_channel][0]["sent"] + (1 if actual_channel <= channel else 0)
                self.assertEqual(current[actual_channel][0]["sent"], expected)
                self.assertEqual(current[actual_channel][1:], before[actual_channel][1:])
            self.assertEqual(self.ui()["tx_scope_mask"], 3)
        for channel in (0, 1):
            current = deepcopy(self.state()["work"]["tx"])
            self.post("/api/tx/edit", ch=channel, index=2)
            self.assertEqual(self.ui()["page"], "txedit")
            self.assertEqual((self.ui()["editor"]["ch"], self.ui()["editor"]["entry"]), (channel, 2))
            self.assertEqual(self.ui()["editor"]["id"], current[channel][2]["id"])
            self.tap("取消")
            self.assertEqual(self.ui()["page"], "tx")
            self.assertEqual(self.ui()["tx_scope_mask"], 3)
            self.assertEqual(self.state()["work"]["tx"], current)

    def test_unified_target_validation_generation_and_drafts_reject_atomically(self):
        self.navigate("tx")
        generation = self.state()["sim"]["generation"]
        self.post("/api/tx/target", mask=2, generation=generation)
        before = deepcopy(self.state())
        for mask in (0, 4, True):
            status, result = self.request("/api/tx/target", {"mask": mask, "generation": generation})
            self.assertEqual(status, 400, result)
            self.assertEqual(self.ui(), before["dev"]["ui"])
            self.assertEqual(self.state()["work"], before["work"])
        status, result = self.request("/api/tx/target", {"mask": 1, "generation": generation - 1})
        self.assertEqual(status, 409, result)
        self.assertEqual(self.ui(), before["dev"]["ui"])
        self.assertEqual(self.state()["work"], before["work"])
        self.post("/api/tx/edit", ch=1, index=0)
        draft = deepcopy(self.state())
        status, result = self.request("/api/tx/target", {"mask": 1, "generation": generation})
        self.assertEqual(status, 409, result)
        self.assertEqual(self.ui(), draft["dev"]["ui"])
        self.assertEqual(self.state()["work"], draft["work"])
        self.tap("取消")
        self.navigate("live")
        before = deepcopy(self.state())
        status, result = self.request("/api/tx/target", {"mask": 3, "generation": generation})
        self.assertEqual(status, 409, result)
        self.assertEqual(self.ui(), before["dev"]["ui"])
        self.assertEqual(self.state()["work"], before["work"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
