"""Random-port HTTP checks for Unicode backspace holds and CAN display tabs."""
from copy import deepcopy
import unittest

import test_web as transport
import test_menu_workflow as menu_module


class HoldAndChannelTests(unittest.TestCase):
    setUpClass = classmethod(menu_module.MenuWorkflowTests.setUpClass.__func__)
    tearDownClass = classmethod(menu_module.MenuWorkflowTests.tearDownClass.__func__)
    setUp, restore_store = menu_module.MenuWorkflowTests.setUp, menu_module.MenuWorkflowTests.restore_store
    request, post, state = menu_module.MenuWorkflowTests.request, menu_module.MenuWorkflowTests.post, menu_module.MenuWorkflowTests.state
    boot, advance, key, navigate = menu_module.MenuWorkflowTests.boot, menu_module.MenuWorkflowTests.advance, menu_module.MenuWorkflowTests.key, menu_module.MenuWorkflowTests.navigate
    ui, item_index, tap, record = menu_module.MenuWorkflowTests.ui, menu_module.MenuWorkflowTests.item_index, menu_module.MenuWorkflowTests.tap, menu_module.MenuWorkflowTests.record
    scope, scope_control, confirm_pending = menu_module.MenuWorkflowTests.scope, menu_module.MenuWorkflowTests.scope_control, menu_module.MenuWorkflowTests.confirm_pending
    configure_groups = menu_module.MenuWorkflowTests.configure_groups

    def generation(self):
        return self.state()["sim"]["generation"]

    def keyboard(self):
        return self.ui()["keyboard"]

    def open_name(self, *parts):
        self.post("/api/plan/open", mode=1, ch=0, generation=self.generation())
        self.assertEqual(self.ui()["page"], "local_plan_name")
        self.tap("清空")
        for part in parts:
            self.tap(part)
        return self.keyboard()["token"]

    def hold(self, action, token=None):
        return self.post("/api/local/backspace", action=action,
                         token=self.keyboard()["token"] if token is None else token,
                         generation=self.generation())

    def channel(self, channel):
        self.post("/api/ui", action="live_channel", channel=channel, generation=self.generation())
        self.assertEqual(self.ui()["live"]["channel"], channel)

    def test_unicode_hold_initial_delay_repeat_and_release_are_exact(self):
        self.open_name("路试", "台架", "A")
        self.assertEqual(self.keyboard()["value"], "路试台架A")
        self.assertEqual((self.keyboard()["backspace_delay_ms"], self.keyboard()["backspace_period_ms"]), (400,90))
        self.hold("begin")
        self.assertEqual(self.keyboard()["value"], "路试台架")
        self.assertTrue(self.keyboard()["backspace_holding"])
        self.hold("begin")  # A duplicate pointerdown cannot delete a second char.
        self.assertEqual(self.keyboard()["value"], "路试台架")
        self.advance(399)
        self.assertEqual(self.keyboard()["value"], "路试台架")
        self.advance(1)
        self.assertEqual(self.keyboard()["value"], "路试台")
        self.advance(89)
        self.assertEqual(self.keyboard()["value"], "路试台")
        self.advance(1)
        self.assertEqual(self.keyboard()["value"], "路试")
        self.hold("end")
        self.assertEqual(self.keyboard()["value"], "路试")
        self.assertFalse(self.keyboard()["backspace_holding"])
        self.advance(500)
        self.assertEqual(self.keyboard()["value"], "路试")

    def test_short_hold_end_and_later_text_never_receive_a_trailing_delete(self):
        self.open_name("A", "B", "C")
        self.hold("begin")
        self.advance(100)
        self.hold("end")
        self.hold("end")
        self.assertEqual(self.keyboard()["value"], "AB")
        self.tap("X")
        self.advance(600)
        self.assertEqual(self.keyboard()["value"], "ABX")
        self.assertFalse(self.keyboard()["backspace_holding"])

    def test_old_hold_cannot_delete_or_stop_another_draft_even_after_reset(self):
        old = self.open_name("A", "B", "C")
        self.hold("begin", old)
        self.tap("取消")
        new = self.open_name("N", "E", "X", "T")
        self.assertNotEqual(old, new)
        self.hold("begin", new)
        self.assertEqual(self.keyboard()["value"], "NEX")
        for action in ("begin", "end"):
            status, result = self.request("/api/local/backspace", {
                "action":action,"token":old,"generation":self.generation()})
            self.assertEqual(status,409,result)
        self.assertTrue(self.keyboard()["backspace_holding"])
        self.advance(400)
        self.assertEqual(self.keyboard()["value"], "NE")
        self.hold("end", new)
        self.tap("取消")
        self.post("/api/sim", reset=True, pause=True)
        self.boot()
        latest = self.open_name("S", "A", "F", "E")
        self.assertNotIn(latest, (old,new))
        for token in (old,new):
            status, result = self.request("/api/local/backspace", {
                "action":"begin","token":token,"generation":self.generation()})
            self.assertEqual(status,409,result)
        self.advance(500)
        self.assertEqual(self.keyboard()["value"], "SAFE")

    def test_typing_another_key_stops_continuous_backspace(self):
        self.open_name("A", "B", "C", "D")
        self.hold("begin")
        self.assertEqual(self.keyboard()["value"], "ABC")
        self.tap("X")
        self.assertFalse(self.keyboard()["backspace_holding"])
        self.advance(600)
        self.assertEqual(self.keyboard()["value"], "ABCX")

    def test_browser_hold_lease_expires_and_renewal_preserves_repeat_timing(self):
        self.open_name(*("A" * 40))
        self.assertEqual(self.keyboard()["backspace_lease_ms"],1500)
        self.hold("begin")
        self.advance(1500)
        self.assertFalse(self.keyboard()["backspace_holding"])
        remaining = self.keyboard()["value"]
        self.assertTrue(remaining)  # A missing pointerup cannot empty a long name.
        self.advance(600)
        self.assertEqual(self.keyboard()["value"],remaining)
        self.tap("取消")
        self.open_name(*("A" * 40))
        self.hold("begin")
        self.advance(300)
        self.hold("begin")  # Renewal neither deletes nor restarts initial delay.
        self.assertEqual(len(self.keyboard()["value"]),39)
        self.advance(99)
        self.assertEqual(len(self.keyboard()["value"]),39)
        self.advance(1)
        self.assertEqual(len(self.keyboard()["value"]),38)
        self.advance(90)
        self.assertEqual(len(self.keyboard()["value"]),37)
        self.advance(1010)
        self.assertTrue(self.keyboard()["backspace_holding"])
        self.hold("end")

    def test_physical_ok_long_press_repeats_and_release_does_not_delete_again(self):
        self.open_name(*"ABCDEFGHIJ")
        self.tap("退格")
        self.assertEqual(self.keyboard()["value"], "ABCDEFGHI")
        self.post("/api/key", k=4, down=True)
        self.advance(999)
        self.assertEqual(self.keyboard()["value"], "ABCDEFGHI")
        self.advance(1)
        self.assertEqual(self.keyboard()["value"], "ABCDEFGH")
        self.advance(90)
        self.assertEqual(self.keyboard()["value"], "ABCDEFG")
        self.post("/api/key", k=4, down=False)
        self.advance(300)
        self.assertEqual(self.keyboard()["value"], "ABCDEFG")
        self.assertFalse(self.keyboard()["backspace_holding"])

    def test_channel_tabs_preserve_both_histories_and_do_not_change_global_filter(self):
        for channel in range(2):
            self.post("/api/env", cmd="bus_fps", a=channel, b=0)
        self.navigate("live")
        self.assertEqual(self.ui()["live"]["channel"], 1)
        before_filter = deepcopy(self.state()["work"]["filter"])
        one = deepcopy(self.ui()["live"]["rows"])
        self.assertTrue(one)
        self.assertTrue(all(row["ch"] == 1 for row in one))
        self.channel(2)
        two = deepcopy(self.ui()["live"]["rows"])
        self.assertTrue(two)
        self.assertTrue(all(row["ch"] == 2 for row in two))
        self.assertTrue(set(row["seq"] for row in one).isdisjoint(row["seq"] for row in two))
        self.channel(1)
        self.assertEqual(self.ui()["live"]["rows"], one)
        self.channel(2)
        self.assertEqual(self.ui()["live"]["rows"], two)
        self.assertEqual(self.state()["work"]["filter"], before_filter)
        self.assertTrue(all(entry["sent"] == 0 for entries in self.state()["work"]["tx"] for entry in entries))

    def test_tabs_do_not_interrupt_either_channel_transmission_or_recording(self):
        self.configure_groups()
        self.navigate("tx")
        self.scope(3)
        self.scope_control("start",3)
        self.confirm_pending()
        for channel in range(2):
            self.post("/api/env", cmd="bus_fps", a=channel, b=30)
        self.navigate("live")
        self.record("start")
        self.advance(200)
        before = self.state()
        for channel in (2,1,2,1):
            self.channel(channel)
            self.advance(50)
        after = self.state()
        self.assertEqual(after["work"]["filter"], before["work"]["filter"])
        self.assertEqual(after["dev"]["rec"]["sid"],2)
        self.assertGreater(after["dev"]["rec"]["frames"], before["dev"]["rec"]["frames"])
        for channel in range(2):
            self.assertEqual(after["work"]["sequence"][channel]["state"],3)
            self.assertGreater(after["work"]["sequence"][channel]["sent"], before["work"]["sequence"][channel]["sent"])
        self.record("stop")
        self.advance(300)
        transport.finish_record_save(self,"保留默认名")
        file = self.state()["work"]["files"][-1]
        status, body = self.request(f"/api/log/export?index={file['index']}")
        self.assertEqual(status,200,body)
        self.assertIn(b"can0 ",body)
        self.assertIn(b"can1 ",body)

    def test_paused_tabs_keep_independent_cursor_and_detail_returns_to_original_tab(self):
        self.navigate("live")
        self.post("/api/ui",action="live_pause",paused=True)
        self.key(0)
        one = deepcopy(self.ui()["live"])
        self.assertEqual(one["cur"],1)
        self.channel(2)
        self.key(0)
        self.key(0)
        two = deepcopy(self.ui()["live"])
        self.assertEqual(two["cur"],2)
        self.channel(1)
        restored = self.ui()["live"]
        self.assertTrue(restored["paused"])
        self.assertEqual(restored["cur"],one["cur"])
        row = next(row for row in restored["rows"] if row["i"] == restored["cur"])
        self.post("/api/ui",action="live_detail",seq=row["seq"])
        detail = deepcopy(self.ui()["detail"])
        status, result = self.request("/api/ui",{"action":"live_channel","channel":2,"generation":self.generation()})
        self.assertEqual(status,409,result)
        self.advance(200)
        self.assertEqual(self.ui()["detail"],detail)
        self.key(5)
        returned = self.ui()["live"]
        self.assertEqual((returned["channel"],returned["cur"],returned["paused"]),(1,1,True))
        self.channel(2)
        self.assertEqual(self.ui()["live"]["cur"],two["cur"])
        self.assertEqual(self.ui()["live"]["rows"],two["rows"])

    def test_same_id_rx_and_tx_are_separate_rows_with_explicit_directions(self):
        entry = self.state()["work"]["tx"][0][0]
        self.post("/api/tx-entry",ch=0,index=0,id="C9",flags=0,len=8,data="10 20 30 40 50 60 70 80",period=0,revision=entry["revision"])
        self.navigate("tx")
        self.scope(1)
        self.scope_control("single",1)
        self.confirm_pending()
        self.advance(20)
        self.navigate("live")
        self.post("/api/ui",action="live_view",view=1)
        rows = [row for row in self.ui()["live"]["rows"] if row["id"] == 0xC9]
        self.assertEqual(len(rows),2,rows)
        self.assertEqual({row["direction"] for row in rows},{"RX","TX"})
        self.assertEqual({row["tx"] for row in rows},{False,True})
        for row in rows:
            self.assertEqual(row["ch"],1)
            self.assertEqual(row["direction"],"TX" if row["tx"] else "RX")
            self.assertFalse(row["err"])
            self.post("/api/ui",action="live_detail",index=row["i"])
            self.assertEqual(self.ui()["detail"]["tx"],row["tx"])
            self.assertEqual(self.ui()["detail"]["ch"],row["ch"])
            self.key(5)

    def test_channel_and_hold_request_validation_and_old_generation_are_atomic(self):
        self.navigate("live")
        for channel in (0,3,True):
            before = deepcopy(self.ui())
            status, result = self.request("/api/ui",{"action":"live_channel","channel":channel,"generation":self.generation()})
            self.assertEqual(status,400,result)
            self.assertEqual(self.ui(),before)
        old_generation = self.generation()
        token = self.open_name("A","B")
        for replacement in ({"token":0},{"token":True},{"action":"invalid"},{"generation":True}):
            before = deepcopy(self.keyboard())
            status, result = self.request("/api/local/backspace",{
                "action":"begin","token":token,"generation":old_generation,**replacement})
            self.assertEqual(status,400,result)
            self.assertEqual(self.keyboard(),before)
        self.post("/api/sim",reset=True,pause=True)
        self.boot()
        self.navigate("live")
        status, result = self.request("/api/ui",{"action":"live_channel","channel":2,"generation":old_generation})
        self.assertEqual(status,409,result)
        status, result = self.request("/api/local/backspace",{"action":"begin","token":token,"generation":old_generation})
        self.assertEqual(status,409,result)
        self.assertEqual(self.ui()["live"]["channel"],1)


if __name__ == "__main__":
    unittest.main(verbosity=2)
