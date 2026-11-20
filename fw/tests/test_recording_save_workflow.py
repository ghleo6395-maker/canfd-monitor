"""Real HTTP tests for automatic local recording naming and simplified send.

Each journey explicitly completes or cancels naming. Global HTTP/clock helpers
never dismiss drafts, and all plan files live in an isolated temporary store.
"""
from copy import deepcopy
import json
import unittest

import test_web as transport
import test_menu_workflow as menu_module


class RecordingSaveWorkflowTests(unittest.TestCase):
    setUpClass = classmethod(menu_module.MenuWorkflowTests.setUpClass.__func__)
    tearDownClass = classmethod(menu_module.MenuWorkflowTests.tearDownClass.__func__)
    setUp, restore_store = menu_module.MenuWorkflowTests.setUp, menu_module.MenuWorkflowTests.restore_store
    request, post, state = menu_module.MenuWorkflowTests.request, menu_module.MenuWorkflowTests.post, menu_module.MenuWorkflowTests.state
    boot, advance, key, navigate = menu_module.MenuWorkflowTests.boot, menu_module.MenuWorkflowTests.advance, menu_module.MenuWorkflowTests.key, menu_module.MenuWorkflowTests.navigate
    ui, item_index, tap, record = menu_module.MenuWorkflowTests.ui, menu_module.MenuWorkflowTests.item_index, menu_module.MenuWorkflowTests.tap, menu_module.MenuWorkflowTests.record
    import_project = menu_module.MenuWorkflowTests.import_project

    def name_parts(self, *parts):
        self.tap("清空")
        for part in parts:
            if part in (".log", ".txt"):
                self.tap(part)
            else:
                for character in part:
                    self.tap(character)

    def record_and_stop(self):
        self.navigate("live")
        self.record("start")
        self.advance(250)
        running = self.state()
        self.assertEqual(running["dev"]["rec"]["sid"], 2)
        file = deepcopy(running["work"]["files"][-1])
        self.record("stop")
        self.assertEqual(self.state()["dev"]["rec"]["sid"], 3)
        for path, payload in (("/api/record", {"action":"start"}),
                              ("/api/ui", {"action":"navigate", "page":"send"})):
            status, result = self.request(path, payload)
            self.assertEqual(status, 409, result)
        self.advance(300)
        self.assertEqual(self.ui()["page"], "local_record_save")
        self.assertEqual(self.state()["dev"]["rec"]["sid"], 0)
        return file

    def file_for(self, token):
        return next(file for file in self.state()["work"]["files"] if file["token"] == token)

    def exported(self, file):
        status, body = self.request(f"/api/log/export?index={file['index']}&token={file['token']}&generation={self.state()['sim']['generation']}")
        self.assertEqual(status, 200, body)
        return body

    def test_local_stop_prompts_after_close_and_custom_full_filename_preserves_bytes(self):
        file = self.record_and_stop()
        closed = self.file_for(file["token"])
        self.assertFalse(closed["writing"])
        self.assertEqual(self.ui()["keyboard"]["value"], closed["name"])
        self.assertEqual(self.ui()["record_token"], file["token"])
        self.assertEqual(self.ui()["record_session"], self.state()["dev"]["rec"]["session"])
        before = self.exported(closed)
        self.assertGreater(len(before), 0)  # Closing saved the bytes before naming.
        self.name_parts("Road", ".txt")
        self.tap("保存")
        saved = self.file_for(file["token"])
        self.assertEqual(saved["name"], "Road.txt")
        self.assertEqual(self.exported(saved), before)
        self.assertEqual(self.ui()["page"], "live")
        generation = self.state()["sim"]["generation"]
        self.record("start")
        self.advance(250)
        self.assertEqual(self.state()["dev"]["rec"]["sid"], 2)
        self.assertEqual(self.state()["sim"]["generation"], generation)
        self.assertNotEqual(self.state()["work"]["files"][-1]["token"], file["token"])

    def test_reserved_and_duplicate_names_keep_draft_and_all_existing_bytes(self):
        self.post("/api/log/import", name="Taken.log", text="(0.000000) can0 123#AABB\n")
        occupied = deepcopy(self.state()["work"]["files"][-1])
        occupied_bytes = self.exported(occupied)
        file = self.record_and_stop()
        original = self.file_for(file["token"])
        original_bytes = self.exported(original)
        for stem in ("CON", "Taken"):
            with self.subTest(name=stem):
                self.name_parts(stem, ".log")
                self.tap("保存")
                self.assertEqual(self.ui()["page"], "local_record_save")
                self.assertEqual(self.ui()["keyboard"]["value"], stem + ".log")
                self.assertEqual(self.file_for(file["token"])["name"], original["name"])
                self.assertEqual(self.exported(self.file_for(file["token"])), original_bytes)
                self.assertEqual(self.exported(self.file_for(occupied["token"])), occupied_bytes)
        self.name_parts("Fresh")
        self.tap("保存")
        self.assertEqual(self.file_for(file["token"])["name"], "Fresh.log")

    def test_keep_default_dismisses_only_by_explicit_choice_and_allows_next_recording(self):
        file = self.record_and_stop()
        original = self.file_for(file["token"])
        self.name_parts("Unsaved")
        before = deepcopy(self.state())
        for path, payload in (("/api/record", {"action":"start"}),
                              ("/api/ui", {"action":"navigate", "page":"send"}),
                              ("/api/log/rename", {"index":file["index"],"token":file["token"],"name":"Extern.log"})):
            status, result = self.request(path, {**payload, "generation":before["sim"]["generation"]})
            self.assertIn(status, (409,422), result)
            self.assertEqual(self.ui()["page"], "local_record_save")
            self.assertEqual(self.ui()["keyboard"]["value"], "Unsaved")
            self.assertEqual(self.state()["work"]["files"], before["work"]["files"])
        self.tap("保留默认名")
        self.assertEqual(self.file_for(file["token"])["name"], original["name"])
        self.assertEqual(self.ui()["page"], "live")
        self.record("start")
        self.advance(250)
        self.assertEqual(self.state()["dev"]["rec"]["sid"], 2)

    def test_back_keeps_default_and_restores_paused_live_context(self):
        self.navigate("live")
        self.post("/api/ui", action="live_view", view=1)
        self.post("/api/ui", action="live_pause", paused=True)
        original = deepcopy(self.ui()["live"])
        self.record("start")
        self.advance(250)
        file = deepcopy(self.state()["work"]["files"][-1])
        self.record("stop")
        self.advance(300)
        self.assertEqual(self.ui()["page"], "local_record_save")
        self.name_parts("Discard")
        self.key(5)
        self.assertEqual(self.ui()["page"], "live")
        returned = self.ui()["live"]
        self.assertEqual((returned["view"],returned["paused"],returned["cur"]),
                         (original["view"],original["paused"],original["cur"]))
        self.assertEqual(returned["rows"], original["rows"])
        self.assertEqual(self.file_for(file["token"])["name"], file["name"])
        self.assertFalse(self.file_for(file["token"])["writing"])

    def test_global_rec_short_press_stops_opens_naming_and_blocks_another_session(self):
        self.navigate("tx")
        self.key(6)
        self.advance(250)
        self.assertEqual(self.ui()["page"], "live")
        file = deepcopy(self.state()["work"]["files"][-1])
        self.key(6)
        self.advance(300)
        self.assertEqual(self.state()["dev"]["rec"]["sid"], 0)
        self.assertEqual(self.ui()["page"], "local_record_save")
        count = len(self.state()["work"]["files"])
        self.key(6)
        self.key(6, 2100)
        self.assertEqual(self.ui()["page"], "local_record_save")
        self.assertEqual(len(self.state()["work"]["files"]), count)
        self.assertEqual(self.state()["dev"]["rec"]["sid"], 0)
        self.key(5)
        self.assertEqual(self.ui()["page"], "live")
        self.assertEqual(self.file_for(file["token"])["name"], file["name"])

    def test_existing_files_are_direct_and_no_user_playback_entry_remains(self):
        self.import_project("One.canplan.json", "Legacy")
        self.navigate("send")
        self.assertEqual(self.ui()["page"], "tx")
        self.assertNotIn("现有报文", [item["l"] for item in self.ui()["items"]])
        self.tap("加载文件")
        self.assertEqual(self.ui()["page"], "send_files")
        names = [item["l"] for item in self.ui()["items"]]
        self.assertIn("One.canplan.json", names)
        self.assertNotIn("Legacy", names)
        self.tap("One.canplan.json")
        self.assertEqual(self.ui()["page"], "tx")
        self.assertFalse(any("回放" in item["l"] for item in self.ui()["items"]))
        self.navigate("files")
        self.assertFalse(any("回放" in item["l"] for item in self.ui()["items"]))
        self.tap(self.state()["work"]["files"][0]["name"])
        self.assertEqual(self.ui()["page"], "file_detail")
        self.assertFalse(any("回放" in item["l"] for item in self.ui()["items"]))
        before = deepcopy(self.state())
        status, result = self.request("/api/play", {"ch":0,"index":0})
        self.assertEqual(status,404,result)
        self.assertEqual(self.state()["work"]["tx"], before["work"]["tx"])
        self.assertFalse(self.state()["dev"]["play_active"])
        for page in ("projects", "project_files", "logplay"):
            status, result = self.request("/api/ui", {"action":"navigate", "page":page})
            self.assertIn(status, (400,409),result)
            self.assertEqual(self.ui(), before["dev"]["ui"])

    def test_remote_confirmed_stop_closes_without_forcing_a_local_name_draft(self):
        self.navigate("live")
        self.record("start")
        self.advance(250)
        self.post("/api/env", cmd="usb", a=1)
        self.post("/api/env", cmd="host", a=1)
        self.advance(10)
        self.post("/api/host", cmd="rec_stop", a=0, b=0)
        self.assertTrue(self.ui()["dlg"].get("kind"))
        self.post("/api/touch", kind=2, a=0)
        self.advance(300)
        self.assertEqual(self.state()["dev"]["rec"]["sid"], 0)
        self.assertEqual(self.ui()["page"], "live")
        self.assertFalse(self.state()["work"]["files"][-1]["writing"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
