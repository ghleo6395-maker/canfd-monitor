"""Per-channel send switches and short-REC workflows over isolated real HTTP.

The harness uses a random port and manually stepped time. User recordings and
port 8765/8766 are never read, modified or restarted.
"""
from copy import deepcopy
import unittest
from urllib.parse import urlencode

import test_web as transport
import test_menu_workflow as menus


class CanSwitchRecKeyTests(unittest.TestCase):
    setUpClass = classmethod(menus.MenuWorkflowTests.setUpClass.__func__)
    tearDownClass = classmethod(menus.MenuWorkflowTests.tearDownClass.__func__)
    setUp, restore_store = menus.MenuWorkflowTests.setUp, menus.MenuWorkflowTests.restore_store
    request, post, state = menus.MenuWorkflowTests.request, menus.MenuWorkflowTests.post, menus.MenuWorkflowTests.state
    boot, advance, key, navigate = menus.MenuWorkflowTests.boot, menus.MenuWorkflowTests.advance, menus.MenuWorkflowTests.key, menus.MenuWorkflowTests.navigate
    ui, item_index, tap = menus.MenuWorkflowTests.ui, menus.MenuWorkflowTests.item_index, menus.MenuWorkflowTests.tap
    configure_groups, confirm_pending = menus.MenuWorkflowTests.configure_groups, menus.MenuWorkflowTests.confirm_pending

    def generation(self):
        return self.state()["sim"]["generation"]

    def channel(self, ch, enabled):
        return self.post("/api/tx/channel", ch=ch, enabled=enabled, generation=self.generation())

    def start_channel(self, ch):
        result = self.channel(ch, True)
        if result.get("pending_confirmation"):
            self.confirm_pending()
        self.advance(20)
        self.assertEqual(self.state()["work"]["sequence"][ch]["state"], 3)

    def file_for(self, token):
        return next(file for file in self.state()["work"]["files"] if file["token"] == token)

    def exported(self, file):
        query = urlencode({"index": file["index"], "token": file["token"], "generation": self.generation()})
        status, content = self.request("/api/log/export?" + query)
        self.assertEqual(status, 200, content)
        return content

    def custom_name(self, stem):
        self.assertEqual(self.ui()["page"], "local_record_save")
        self.tap("清空")
        for character in stem:
            self.tap(character)
        self.tap("保存")

    def assert_no_tx(self):
        state = self.state()
        self.assertTrue(all(entry["sent"] == 0 and not entry["active"]
                            for channel in state["work"]["tx"] for entry in channel))
        self.assertTrue(all(channel["state"] == 0 for channel in state["work"]["sequence"]))

    def test_channel_on_and_off_are_idempotent_without_resetting_the_sender(self):
        self.navigate("tx")
        self.configure_groups()
        self.start_channel(0)
        before = deepcopy(self.state())
        request = before["work"]["tx_scope"]["request"]
        self.assertEqual(self.channel(0, True)["rc"], 0)
        self.assertEqual(self.channel(0, True)["rc"], 0)
        after = self.state()
        self.assertEqual(after["work"]["sequence"], before["work"]["sequence"])
        self.assertEqual(after["work"]["tx"], before["work"]["tx"])
        self.assertEqual((self.ui()["tx_scope_mask"], self.ui()["tx_channel"]),
                         (before["dev"]["ui"]["tx_scope_mask"], before["dev"]["ui"]["tx_channel"]))
        self.advance(60)
        running = self.state()["work"]["sequence"][0]
        self.assertGreater(running["sent"], before["work"]["sequence"][0]["sent"])
        self.assertEqual(self.state()["work"]["tx_scope"]["request"], request)
        self.channel(0, False)
        stopped = deepcopy(self.state()["work"])
        self.assertEqual(stopped["sequence"][0]["state"], 0)
        self.channel(0, False)
        self.assertEqual(self.state()["work"], stopped)
        self.advance(60)
        self.assertEqual(self.state()["work"]["sequence"][0]["sent"], stopped["sequence"][0]["sent"])
        self.assertTrue(all(entry["sent"] == 0 for entry in self.state()["work"]["tx"][1]))

    def test_channel_switch_only_stops_its_channel_and_other_loop_continues(self):
        self.navigate("tx")
        self.configure_groups()
        self.start_channel(0)
        first = deepcopy(self.state()["work"]["sequence"][0])
        self.start_channel(1)
        self.assertEqual(self.state()["work"]["sequence"][0]["revision"], first["revision"])
        self.assertGreaterEqual(self.state()["work"]["sequence"][0]["sent"], first["sent"])
        before = deepcopy(self.state()["work"])
        self.channel(0, False)
        self.channel(0, False)
        self.assertEqual(self.state()["work"]["sequence"][1], before["sequence"][1])
        self.advance(80)
        after = self.state()["work"]
        self.assertEqual(after["sequence"][0]["state"], 0)
        self.assertEqual(after["sequence"][0]["sent"], before["sequence"][0]["sent"])
        self.assertEqual(after["sequence"][1]["state"], 3)
        self.assertGreater(after["sequence"][1]["sent"], before["sequence"][1]["sent"])
        self.assertEqual(after["sequence"][1]["revision"], before["sequence"][1]["revision"])
        self.channel(1, False)

    def test_modal_requests_do_not_duplicate_and_arming_can_be_cancelled(self):
        self.navigate("tx")
        self.configure_groups()
        self.assertTrue(self.channel(0, True)["pending_confirmation"])
        modal = deepcopy(self.state())
        self.assertTrue(self.ui()["dlg"].get("kind"))
        for enabled in (True, False):
            status, result = self.request("/api/tx/channel", {"ch": 0, "enabled": enabled,
                                                            "generation": self.generation()})
            self.assertEqual(status, 409, result)
            self.assertEqual(self.ui()["dlg"], modal["dev"]["ui"]["dlg"])
            self.assertEqual(self.state()["work"], modal["work"])
        self.post("/api/touch", kind=2, a=0, generation=self.generation())
        arming = deepcopy(self.state()["work"])
        self.assertEqual(arming["tx_scope"]["state"], 2)
        repeated = self.channel(0, True)
        self.assertTrue(repeated["pending_confirmation"])
        self.assertEqual(self.state()["work"], arming)
        self.channel(0, False)
        self.advance(40)
        self.assertEqual(self.state()["work"]["tx_scope"]["state"], 0)
        self.assert_no_tx()

    def test_channel_validation_generation_and_local_draft_guards_are_atomic(self):
        self.navigate("tx")
        before = deepcopy(self.state())
        for payload in ({"ch": -1, "enabled": True}, {"ch": 2, "enabled": True},
                        {"ch": True, "enabled": True}, {"ch": 0, "enabled": 1},
                        {"ch": 0, "enabled": "true"}, {"ch": 0, "enabled": None}):
            status, result = self.request("/api/tx/channel", {**payload, "generation": self.generation()})
            self.assertEqual(status, 400, result)
            self.assertEqual(self.ui(), before["dev"]["ui"])
            self.assertEqual(self.state()["work"], before["work"])
        status, _ = self.request("/api/tx/channel", {"ch": 0, "enabled": True,
                                                   "generation": self.generation() - 1})
        self.assertEqual(status, 409)
        self.assertEqual(self.state()["work"], before["work"])
        self.post("/api/tx/edit", ch=0, index=0)
        draft = deepcopy(self.state())
        for enabled in (True, False):
            status, _ = self.request("/api/tx/channel", {"ch": 0, "enabled": enabled,
                                                       "generation": self.generation()})
            self.assertEqual(status, 409)
            self.assertEqual(self.ui(), draft["dev"]["ui"])
            self.assertEqual(self.state()["work"], draft["work"])
        self.tap("取消")
        self.navigate("live")
        before = deepcopy(self.state())
        status, _ = self.request("/api/tx/channel", {"ch": 0, "enabled": True,
                                                   "generation": self.generation()})
        self.assertEqual(status, 409)
        self.assertEqual(self.ui(), before["dev"]["ui"])
        self.assertEqual(self.state()["work"], before["work"])

    def test_pending_single_and_legacy_periodic_sender_keep_their_ownership(self):
        self.navigate("tx")
        self.configure_groups()
        self.post("/api/tx/scope/control", action="single", mask=1, index=0,
                  generation=self.generation())
        self.post("/api/touch", kind=2, a=0, generation=self.generation())
        pending = deepcopy(self.state()["work"])
        self.assertEqual(pending["tx_scope"]["state"], 2)
        status, _ = self.request("/api/tx/channel", {"ch": 0, "enabled": True,
                                                   "generation": self.generation()})
        self.assertEqual(status, 409)
        self.assertEqual(self.state()["work"], pending)
        self.channel(0, False)
        self.advance(30)
        self.assert_no_tx()
        self.post("/api/tx/select", ch=0, index=0, generation=self.generation())
        self.key(3)  # The retained physical-right action enables this slot's period.
        self.confirm_pending()
        self.advance(20)
        legacy = deepcopy(self.state()["work"])
        self.assertTrue(legacy["tx"][0][0]["active"])
        self.assertEqual(legacy["sequence"][0]["state"], 0)
        self.channel(0, True)
        self.assertEqual(self.state()["work"], legacy)
        self.channel(0, False)
        self.assertFalse(self.state()["work"]["tx"][0][0]["active"])
        self.assertEqual(self.state()["work"]["sequence"][0]["state"], 0)

    def test_second_short_rec_stops_and_custom_save_preserves_recorded_bytes(self):
        self.navigate("live")
        generation = self.generation()
        self.key(6)
        self.advance(250)
        running = self.state()
        self.assertEqual(running["dev"]["rec"]["sid"], 2)
        self.assertEqual(running["dev"]["rec"]["markers"], 0)
        first = deepcopy(running["work"]["files"][-1])
        count = len(running["work"]["files"])
        self.key(6)
        self.assertEqual(self.state()["dev"]["rec"]["sid"], 3)
        self.assertEqual(self.state()["dev"]["rec"]["markers"], 0)
        self.assertEqual(len(self.state()["work"]["files"]), count)
        self.advance(300)
        closed = self.file_for(first["token"])
        self.assertFalse(closed["writing"])
        before = self.exported(closed)
        self.assertGreater(len(before), 0)
        self.custom_name("DriveA")
        saved = self.file_for(first["token"])
        self.assertEqual(saved["name"], "DriveA.log")
        self.assertEqual(self.exported(saved), before)
        self.assertEqual(self.ui()["page"], "live")
        self.key(6)
        self.advance(200)
        after = self.state()
        self.assertEqual(after["sim"]["generation"], generation)
        self.assertEqual(after["dev"]["rec"]["sid"], 2)
        self.assertEqual(after["dev"]["rec"]["markers"], 0)
        self.assertNotEqual(after["work"]["files"][-1]["token"], first["token"])
        self.assertEqual(self.file_for(first["token"])["name"], "DriveA.log")
        self.assertEqual(self.exported(self.file_for(first["token"])), before)

    def test_short_rec_cancels_preparing_and_stopping_presses_create_no_new_file(self):
        self.navigate("live")
        self.key(6, 10)
        preparing = self.state()
        self.assertEqual(preparing["dev"]["rec"]["sid"], 1)
        first = preparing["work"]["files"][-1]
        count = len(preparing["work"]["files"])
        session = preparing["dev"]["rec"]["session"]
        self.key(6, 10)
        self.assertEqual(self.state()["dev"]["rec"]["sid"], 3)
        self.key(6, 10)
        self.assertEqual(self.state()["dev"]["rec"]["sid"], 3)
        self.assertEqual(self.state()["dev"]["rec"]["session"], session)
        self.assertEqual(self.state()["dev"]["rec"]["markers"], 0)
        self.assertEqual(len(self.state()["work"]["files"]), count)
        self.advance(300)
        self.assertEqual(self.ui()["page"], "local_record_save")
        self.assertFalse(self.file_for(first["token"])["writing"])
        transport.finish_record_save(self, "保留默认名")
        self.key(6, 10)
        self.assertEqual(self.state()["dev"]["rec"]["sid"], 1)
        self.assertEqual(len(self.state()["work"]["files"]), count + 1)
        self.assertNotEqual(self.state()["work"]["files"][-1]["token"], first["token"])

    def test_short_rec_keeps_background_can_and_restores_paused_live_after_naming(self):
        self.navigate("tx")
        self.configure_groups()
        self.start_channel(1)
        self.key(6)
        self.advance(250)
        self.assertEqual(self.ui()["page"], "live")
        self.post("/api/ui", action="live_channel", channel=2, generation=self.generation())
        self.post("/api/ui", action="live_pause", paused=True, generation=self.generation())
        paused = deepcopy(self.ui()["live"])
        file = deepcopy(self.state()["work"]["files"][-1])
        self.key(6)
        self.advance(300)
        self.assertEqual(self.ui()["page"], "local_record_save")
        before = self.exported(self.file_for(file["token"]))
        self.assertGreater(len(before), 0)
        sent = self.state()["work"]["sequence"][1]["sent"]
        count = len(self.state()["work"]["files"])
        self.key(6)
        self.advance(100)
        self.assertEqual(self.ui()["page"], "local_record_save")
        self.assertEqual(len(self.state()["work"]["files"]), count)
        self.assertGreater(self.state()["work"]["sequence"][1]["sent"], sent)
        self.custom_name("KeepTx")
        restored = self.ui()["live"]
        self.assertEqual((restored["channel"], restored["view"], restored["paused"], restored["cur"]),
                         (paused["channel"], paused["view"], True, paused["cur"]))
        self.assertEqual(restored["rows"], paused["rows"])
        self.assertEqual(self.exported(self.file_for(file["token"])), before)
        self.assertEqual(self.state()["work"]["sequence"][1]["state"], 3)

    def test_rec_key_does_not_consume_an_open_input_or_name_draft(self):
        self.navigate("tx")
        self.post("/api/tx/edit", ch=0, index=0)
        self.tap("报文 ID")
        keyboard = deepcopy(self.ui()["keyboard"])
        count = len(self.state()["work"]["files"])
        self.key(6)
        self.assertEqual(self.ui()["keyboard"], keyboard)
        self.assertEqual(self.state()["dev"]["rec"]["sid"], 0)
        self.assertEqual(len(self.state()["work"]["files"]), count)
        self.tap("取消")
        self.tap("取消")
        self.key(6)
        self.advance(200)
        self.key(6)
        self.advance(300)
        self.assertEqual(self.ui()["page"], "local_record_save")
        keyboard = deepcopy(self.ui()["keyboard"])
        file = deepcopy(self.state()["work"]["files"][-1])
        self.key(6)
        self.key(6, 2100)
        self.assertEqual(self.ui()["keyboard"], keyboard)
        self.assertEqual(len(self.state()["work"]["files"]), count + 1)
        self.assertEqual(self.state()["dev"]["rec"]["markers"], 0)
        self.assertEqual(self.file_for(file["token"]), file)
        transport.finish_record_save(self, "保留默认名")


if __name__ == "__main__":
    unittest.main(verbosity=2)
