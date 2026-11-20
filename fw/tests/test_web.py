"""HTTP regressions use an isolated in-process server with a manually stepped clock."""
import concurrent.futures
import base64
import http.client
import importlib.util
import json
from pathlib import Path
import threading
import unittest

spec = importlib.util.spec_from_file_location("sim_web", Path(__file__).parents[1] / "sim/web/server.py")
web = importlib.util.module_from_spec(spec)
spec.loader.exec_module(web)


def finish_record_save(test, choice="保存"):
    """Explicitly finish a successful local stop's naming step.

    Call only in journeys that intentionally accept the default name or keep it
    on cancel. Request/clock helpers never dismiss drafts automatically.
    """
    ui = test.state()["dev"]["ui"]
    test.assertEqual(ui["page"], "local_record_save", ui)
    row = next(index for index, item in enumerate(ui["items"]) if item["l"] == choice)
    test.post("/api/touch", kind=0, a=row)
    test.assertNotEqual(test.state()["dev"]["ui"]["page"], "local_record_save")


class WebTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.http = web.ThreadingHTTPServer(("127.0.0.1", 0), web.Handler)
        web.PORT = cls.http.server_port
        cls.thread = threading.Thread(target=cls.http.serve_forever, daemon=True)
        cls.thread.start()

    @classmethod
    def tearDownClass(cls):
        cls.http.shutdown(); cls.http.server_close(); cls.thread.join()

    def request(self, path, obj=None, raw=None, headers=None):
        body = raw if raw is not None else json.dumps(obj).encode() if obj is not None else None
        conn = http.client.HTTPConnection("127.0.0.1", web.PORT, timeout=5)
        h = {"Content-Type": "application/json", **(headers or {})}
        conn.request("POST" if body is not None else "GET", path, body=body, headers=h)
        response = conn.getresponse(); data = response.read(); status = response.status
        content_type = response.getheader("Content-Type", "")
        conn.close()
        return status, json.loads(data) if "json" in content_type else data

    def post(self, path, **obj):
        status, result = self.request(path, obj)
        self.assertEqual(status, 200, result)
        return result

    def state(self):
        status, state = self.request("/api/state")
        self.assertEqual(status, 200)
        return state

    def setUp(self):
        self.post("/api/sim", reset=True, pause=True, speed=1)
        self.boot()

    def boot(self):
        self.post("/api/key", k=7, down=True)
        self.post("/api/sim", step=1000)
        self.post("/api/sim", step=250)
        self.post("/api/key", k=7, down=False)
        self.post("/api/sim", step=500)
        self.assertEqual(self.state()["dev"]["power"]["state"], "运行")

    def frame(self, **replace):
        return {"ch":0,"index":0,"id":"123","flags":0,"len":4,"data":"AA BB CC DD","period":100,"revision":0,**replace}

    def advance(self, milliseconds):
        for _ in range(milliseconds // 1000):
            self.post("/api/sim", step=1000)
        if milliseconds % 1000:
            self.post("/api/sim", step=milliseconds % 1000)

    def key(self, key, milliseconds=60):
        self.post("/api/key", k=key, down=True)
        self.advance(milliseconds)
        self.post("/api/key", k=key, down=False)
        self.advance(20)

    def connect_host(self):
        self.post("/api/env", cmd="usb", a=1)
        self.post("/api/env", cmd="host", a=1)
        self.advance(10)

    def navigate(self, page):
        self.post("/api/ui", action="navigate", page=page)
        self.assertEqual(self.state()["dev"]["ui"]["page"], "tx" if page == "send" else page)

    def begin_recording(self):
        self.key(6)
        self.advance(200)
        self.assertEqual(self.state()["dev"]["rec"]["sid"], 2)

    def stop_recording(self):
        self.connect_host()
        self.post("/api/host", cmd="rec_stop", a=0, b=0)
        self.assertTrue(self.state()["dev"]["ui"]["dlg"].get("kind"))
        self.post("/api/touch", kind=2, a=0)
        self.advance(300)
        self.assertEqual(self.state()["dev"]["rec"]["sid"], 0)

    def import_database(self, kind="dbc"):
        source = Path(__file__).parents[1] / "sim/web/examples" / f"review.{kind}"
        return self.post("/api/database/import", name=source.name,
                         content=base64.b64encode(source.read_bytes()).decode(), encoding="auto")

    def apply_database(self, catalog, ch=0, start=0, count=None):
        frames = catalog["frames"] if count is None else catalog["frames"][:count]
        entries = self.state()["work"]["tx"][ch]
        return self.post("/api/database/apply", database=catalog["database"], ch=ch, start_index=start,
                         frames=[frame["key"] for frame in frames],
                         revisions=[entries[start+i]["revision"] for i in range(len(frames))],
                         generation=self.state()["sim"]["generation"])

    def test_valid_edit_shared_state(self):
        self.post("/api/tx-entry", **self.frame(id="18FF50E5",flags=7,len=12,data="00 01 02 03 04 05 06 07 08 09 0A 0B"))
        entry = self.state()["work"]["tx"][0][0]
        self.assertEqual((entry["id"],entry["len"],entry["flags"]),(0x18FF50E5,12,7))
        self.assertEqual(entry["data"],"00 01 02 03 04 05 06 07 08 09 0A 0B")
        self.post("/api/tx/edit",ch=0,index=0)
        self.assertEqual(self.state()["dev"]["ui"]["page"],"txedit")

    def test_invalid_edit_is_atomic(self):
        before = self.state()["work"]["tx"][0][0]
        for change, expected in [({"id":"800"},422),({"flags":4},422),({"data":"A"},400),
                                 ({"len":2},400),({"period":65536},400),({"len":True},400)]:
            status, result = self.request("/api/tx-entry", self.frame(**change))
            self.assertEqual(status,expected,result)
            self.assertEqual(self.state()["work"]["tx"][0][0],before)

    def test_filter_shared_without_recording_mask(self):
        self.post("/api/filter",channel=1,type=0,id="120",mask="7F0",direction=1,format=1)
        state = self.state()
        self.assertEqual(state["work"]["filter"]["mask"],0x7F0)
        self.assertEqual(state["dev"]["ui"]["filter_cfg"]["id"],0x120)

    def test_stale_edit_does_not_overwrite_new_data(self):
        self.post("/api/tx-entry", **self.frame(id="456"))
        status, _ = self.request("/api/tx-entry", self.frame(id="123"))
        self.assertEqual(status,422)
        self.assertEqual(self.state()["work"]["tx"][0][0]["id"],0x456)

    def test_stale_tx_request_after_reset_preserves_new_entry_and_ui(self):
        generation = self.state()["sim"]["generation"]
        self.post("/api/sim", reset=True, pause=True)
        self.boot()
        before = self.state()
        # Entry revisions restart at zero, so revision alone cannot distinguish
        # a delayed request from the simulator instance before reset.
        request = self.frame(id="456", revision=before["work"]["tx"][0][0]["revision"])
        for path, payload in (("/api/tx-entry", request), ("/api/tx/edit", {"ch":0,"index":0})):
            status, result = self.request(path, {**payload,"generation":generation})
            self.assertEqual(status,409,result)
            after = self.state()
            self.assertEqual(after["work"]["tx"],before["work"]["tx"])
            self.assertEqual(after["dev"]["ui"],before["dev"]["ui"])
            for invalid in (True,-1,"0"):
                status, result = self.request(path,{**payload,"generation":invalid})
                self.assertEqual(status,400,result)
                self.assertEqual(self.state()["work"]["tx"],before["work"]["tx"])
        self.post("/api/tx-entry", **request, generation=before["sim"]["generation"])
        self.assertEqual(self.state()["work"]["tx"][0][0]["id"],0x456)

    def test_edit_during_pending_send_confirmation_is_rejected(self):
        self.post("/api/env",cmd="usb",a=1)
        self.post("/api/env",cmd="host",a=1)
        self.post("/api/sim",step=10)
        self.post("/api/host",cmd="tx_send",a=0,b=0)
        before = self.state()["work"]["tx"][0][0]
        status, _ = self.request("/api/tx-entry", self.frame(id="456"))
        self.assertEqual(status,422)
        self.assertEqual(self.state()["work"]["tx"][0][0],before)
        self.assertEqual(self.state()["dev"]["ch"][0]["state"],"只听")

    def test_sim_invalid_request_does_not_mutate(self):
        before = self.state()
        status, _ = self.request("/api/sim", {"speed":2,"pause":"bad"})
        self.assertEqual(status,400)
        status, _ = self.request("/api/sim", {"reset":True,"step":1001})
        self.assertEqual(status,400)
        self.assertEqual(self.state()["dev"]["now"],before["dev"]["now"])
        self.assertEqual(self.state()["sim"],before["sim"])

    def test_import_roundtrip_and_removed_playback_endpoint(self):
        text = "(0.000000) can0 123#AABB\n(0.100000) can1 18FF50E5##1112233445566778899AABBCC\n(0.300000) can0 456#CCDD\n"
        result = self.post("/api/log/import",name="example.log",text=text)
        self.assertEqual(result["frames"],3)
        status, exported = self.request(f"/api/log/export?index={result['index']}")
        self.assertEqual(status,200)
        self.assertEqual(exported.decode(),text)
        status, _ = self.request("/api/play", {"ch":0,"index":result["index"]})
        self.assertEqual(status,404)
        state = self.state()
        self.assertEqual(state["dev"]["ch"][0]["state"],"只听")
        self.assertFalse(state["dev"]["ui"]["dlg"].get("kind"))
        self.assertFalse(state["dev"]["play_active"])

    def test_bad_log_atomic_and_line_number(self):
        before = len(self.state()["work"]["files"])
        status,result = self.request("/api/log/import",{"name":"bad.log","text":"(0.000000) can0 123#AA\n(0.100000) can0 123#GG\n"})
        self.assertEqual(status,422)
        self.assertEqual(result["line"],2)
        self.assertEqual(len(self.state()["work"]["files"]),before)
        status,result = self.request("/api/log/import",{"name":"backwards.log","text":"(1.000000) can0 123#AA\n(0.100000) can0 123#BB\n"})
        self.assertEqual(status,422)
        self.assertEqual(result["line"],2)

    def test_recording_exports_real_bounded_frames(self):
        self.post("/api/key",k=6,down=True)
        self.post("/api/sim",step=60)
        self.post("/api/key",k=6,down=False)
        for _ in range(5): self.post("/api/sim",step=1000)
        state = self.state(); recording = state["work"]["files"][-1]
        self.assertGreater(recording["frames"],512)
        self.assertEqual(recording["sample_frames"],512)
        self.assertTrue(recording["sample_truncated"])
        self.assertGreater(recording["sample_dropped"],0)
        status, _ = self.request(f"/api/log/export?index={recording['index']}")
        self.assertEqual(status,422)  # current recording must close first
        self.key(6)  # A second short REC closes the current session, not a marker.
        self.post("/api/sim",step=300)
        self.assertEqual(self.state()["dev"]["rec"]["markers"], 0)
        self.assertEqual(self.state()["dev"]["ui"]["page"], "local_record_save")
        finish_record_save(self, "保存")
        status,text = self.request(f"/api/log/export?index={recording['index']}")
        self.assertEqual(status,200)
        self.assertEqual(len(text.splitlines()),512)
        self.assertIn(b"can0 ",text); self.assertIn(b"can1 ",text)

    def test_host_requires_connection(self):
        status, _ = self.request("/api/host",{"cmd":"tx_send","a":0,"b":0})
        self.assertEqual(status,409)
        self.post("/api/env",cmd="usb",a=1)
        self.post("/api/env",cmd="host",a=1)
        self.post("/api/sim",step=10)
        self.post("/api/host",cmd="tx_send",a=0,b=0)
        self.assertTrue(self.state()["dev"]["ui"]["dlg"].get("kind"))

    def test_request_validation_and_origin(self):
        for body in [b"[]",b"{",b'{"speed":NaN}',b'{"speed":1e300}']:
            status, _ = self.request("/api/sim",raw=body)
            self.assertEqual(status,400)
        status,_ = self.request("/api/sim",{"pause":True},headers={"Origin":"https://example.org"})
        self.assertEqual(status,403)
        status,_ = self.request("/api/env",{"cmd":"bus_fps","a":0,"b":65536})
        self.assertEqual(status,400)

    def test_pause_step_and_reset(self):
        before = self.state()["dev"]["now"]
        self.post("/api/sim",step=10)
        self.assertEqual(self.state()["dev"]["now"],before+10)
        self.post("/api/sim",reset=True)
        state = self.state()
        self.assertEqual(state["dev"]["now"],0)
        self.assertEqual(state["dev"]["power"]["state"],"关机")
        self.assertEqual(len(state["work"]["files"]),5)

    def test_concurrent_snapshots(self):
        def read(_):
            status, state = self.request("/api/state")
            assert status==200 and len(state["work"]["tx"])==2
            assert state["dev"]["power"]["state"]=="运行"
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            list(pool.map(read, range(64)))

    def test_review_navigation_is_not_an_action(self):
        """Review shortcuts may select a page but must not start its operation."""
        for page in ("send", "send_files", "tx", "rec", "diag", "files", "chset", "sysset", "live", "home"):
            self.navigate(page)
        state = self.state()
        self.assertEqual(state["dev"]["rec"]["sid"], 0)
        self.assertEqual([channel["state"] for channel in state["dev"]["ch"]], ["只听", "只听"])
        self.assertFalse(any(entry["active"] for entries in state["work"]["tx"] for entry in entries))
        self.assertFalse(state["dev"]["ui"]["dlg"].get("kind"))

    def test_review_navigation_preserves_draft_and_confirmation(self):
        before = self.state()["work"]["tx"][0][0]
        self.post("/api/tx/edit", ch=0, index=0)
        self.post("/api/touch", kind=0, a=0)  # Open device ID keyboard.
        keyboard = self.state()["dev"]["ui"]["keyboard"]
        self.post("/api/local/input", action="replace", token=keyboard["token"], text="456",
                  generation=self.state()["sim"]["generation"])
        draft = self.state()["dev"]["ui"]
        status, _ = self.request("/api/ui", {"action":"navigate", "page":"live"})
        self.assertEqual(status, 409)
        after = self.state()["dev"]["ui"]
        self.assertEqual(after["page"], draft["page"])
        self.assertEqual(after["cursor"], draft["cursor"])
        self.assertEqual(after["keyboard"], draft["keyboard"])
        self.assertEqual(self.state()["work"]["tx"][0][0], before)
        self.post("/api/touch", kind=1, a=0)  # Discard keyboard, then parent draft.
        self.post("/api/touch", kind=1, a=0)
        self.connect_host()
        self.post("/api/host", cmd="tx_send", a=0, b=0)
        dialog = self.state()["dev"]["ui"]["dlg"]
        self.assertTrue(dialog.get("kind"))
        status, _ = self.request("/api/ui", {"action":"navigate", "page":"files"})
        self.assertEqual(status, 409)
        self.assertEqual(self.state()["dev"]["ui"]["dlg"], dialog)
        self.assertEqual(self.state()["dev"]["ch"][0]["state"], "只听")

    def test_live_pause_only_freezes_display(self):
        for channel in (0, 1):
            self.post("/api/env", cmd="bus_fps", a=channel, b=100)
        self.begin_recording()
        self.navigate("live")
        self.advance(300)
        self.post("/api/ui", action="live_pause", paused=True)
        before = self.state()
        self.assertTrue(before["dev"]["ui"]["live"]["paused"])
        frozen = before["dev"]["ui"]["live"]["rows"]
        self.assertGreater(len(frozen), 0)
        self.advance(500)
        after = self.state()
        self.assertEqual(after["dev"]["ui"]["live"]["rows"], frozen)
        self.assertGreater(after["dev"]["ui"]["live"]["hidden"], before["dev"]["ui"]["live"]["hidden"])
        self.assertGreater(after["dev"]["rec"]["frames"], before["dev"]["rec"]["frames"])
        self.assertEqual(after["dev"]["rec"]["sid"], 2)
        self.post("/api/ui", action="live_pause", paused=False)
        self.advance(100)
        self.assertFalse(self.state()["dev"]["ui"]["live"]["paused"])
        self.assertNotEqual(self.state()["dev"]["ui"]["live"]["rows"], frozen)

    def test_id_summary_pause_freezes_visible_values_but_not_recording(self):
        self.begin_recording()
        self.navigate("live")
        self.post("/api/ui", action="live_view", view=1)
        self.post("/api/ui", action="live_pause", paused=True)
        before = self.state()
        frozen = before["dev"]["ui"]["live"]["rows"]
        self.assertGreater(len(frozen), 0)
        self.advance(500)
        state = self.state()
        self.assertEqual(state["dev"]["ui"]["live"]["rows"], frozen)
        self.assertGreater(state["dev"]["rec"]["frames"], before["dev"]["rec"]["frames"])
        self.post("/api/ui", action="live_pause", paused=False)
        resumed = self.state()["dev"]["ui"]["live"]["rows"]
        self.assertEqual([(row["ch"], row["id"]) for row in resumed],
                         [(row["ch"], row["id"]) for row in frozen])
        self.assertTrue(any(new["count"] > old["count"] for new, old in zip(resumed, frozen)))

    def test_filter_results_do_not_filter_recording(self):
        for channel in (0, 1):
            self.post("/api/env", cmd="bus_fps", a=channel, b=100)
        self.begin_recording()
        self.navigate("live")
        self.post("/api/filter", channel=1, type=0, id="0", mask="0", direction=0, format=0)
        self.advance(400)
        state = self.state()
        rows = state["dev"]["ui"]["live"]["rows"]
        self.assertGreater(len(rows), 0)
        self.assertEqual({row["ch"] for row in rows}, {1})
        self.assertGreater(state["dev"]["rec"]["frames"], 0)
        recording_index = state["work"]["files"][-1]["index"]
        self.stop_recording()
        status, text = self.request(f"/api/log/export?index={recording_index}")
        self.assertEqual(status, 200)
        self.assertIn(b"can0 ", text)
        self.assertIn(b"can1 ", text)

    def test_record_stop_confirmation_can_be_cancelled(self):
        self.begin_recording()
        self.connect_host()
        self.post("/api/host", cmd="rec_stop", a=0, b=0)
        before = self.state()
        self.assertEqual(before["dev"]["rec"]["sid"], 2)
        self.assertTrue(before["dev"]["ui"]["dlg"].get("kind"))
        self.advance(100)
        self.assertGreater(self.state()["dev"]["rec"]["frames"], before["dev"]["rec"]["frames"])
        self.post("/api/touch", kind=2, a=1)
        state = self.state()
        self.assertEqual(state["dev"]["rec"]["sid"], 2)
        self.assertFalse(state["dev"]["ui"]["dlg"].get("kind"))

    def test_record_buttons_stop_and_create_a_new_session_without_reboot(self):
        self.navigate("rec")
        initial = self.state()
        files = len(initial["work"]["files"])
        self.assertFalse(initial["dev"]["power"]["host"])
        self.post("/api/record", action="start", generation=initial["sim"]["generation"])
        self.assertEqual(self.state()["dev"]["rec"]["sid"], 1)
        self.advance(300)
        self.post("/api/record", action="mark")
        self.advance(300)
        first = self.state()
        self.assertEqual(first["dev"]["rec"]["sid"], 2)
        self.assertEqual(first["dev"]["rec"]["markers"], 1)
        self.assertEqual(len(first["work"]["files"]), files + 1)
        old_file = first["work"]["files"][-1]
        self.assertTrue(old_file["writing"])
        self.assertGreater(old_file["frames"], 0)
        self.post("/api/record", action="stop")
        self.assertEqual(self.state()["dev"]["rec"]["sid"], 3)
        self.assertFalse(self.state()["dev"]["ui"]["dlg"].get("kind"))
        status, _ = self.request("/api/record", {"action":"start"})
        self.assertEqual(status, 409)  # A close fence must finish first.
        self.advance(300)
        stopped = self.state()
        self.assertEqual(stopped["dev"]["rec"]["sid"], 0)
        old_file = stopped["work"]["files"][-1]
        self.assertFalse(old_file["writing"])
        status, old_text = self.request(f"/api/log/export?index={old_file['index']}")
        self.assertEqual(status, 200)
        finish_record_save(self, "保存")
        self.post("/api/record", action="start")
        self.advance(300)
        new = self.state()
        self.assertEqual(new["dev"]["power"]["state"], "运行")
        self.assertEqual(new["dev"]["rec"]["sid"], 2)
        self.assertEqual(new["dev"]["rec"]["markers"], 0)
        self.assertEqual(new["dev"]["rec"]["session"], first["dev"]["rec"]["session"] + 1)
        self.assertEqual(len(new["work"]["files"]), files + 2)
        self.assertNotEqual(new["work"]["files"][-1]["name"], old_file["name"])
        self.assertNotEqual(new["work"]["files"][-1]["index"], old_file["index"])
        self.assertEqual(new["work"]["files"][-2], old_file)
        self.assertGreater(new["dev"]["rec"]["frames"], 0)
        self.assertLess(new["dev"]["rec"]["frames"], stopped["dev"]["rec"]["frames"])
        status, preserved_text = self.request(f"/api/log/export?index={old_file['index']}")
        self.assertEqual(status, 200)
        self.assertEqual(preserved_text, old_text)

    def test_record_buttons_stop_during_preparation_then_restart(self):
        self.post("/api/record", action="start")
        preparing = self.state()
        self.assertEqual(preparing["dev"]["rec"]["sid"], 1)
        first_index = preparing["work"]["files"][-1]["index"]
        for action in ("start", "mark"):
            status, _ = self.request("/api/record", {"action":action})
            self.assertEqual(status, 409)
        self.post("/api/record", action="stop")
        self.advance(300)
        self.assertEqual(self.state()["dev"]["rec"]["sid"], 0)
        self.assertFalse(self.state()["work"]["files"][-1]["writing"])
        finish_record_save(self, "保存")
        self.post("/api/record", action="start")
        self.advance(300)
        state = self.state()
        self.assertEqual(state["dev"]["rec"]["sid"], 2)
        self.assertNotEqual(state["work"]["files"][-1]["index"], first_index)

    def test_record_buttons_wrong_state_is_rejected_without_mutation(self):
        before = self.state()
        for action in ("stop", "mark"):
            status, result = self.request("/api/record", {"action":action})
            self.assertEqual(status, 409, result)
            self.assertEqual(result["rc"], -8)
            self.assertEqual(self.state()["dev"]["rec"], before["dev"]["rec"])
            self.assertEqual(self.state()["work"]["files"], before["work"]["files"])
        self.post("/api/record", action="start")
        self.advance(300)
        running = self.state()
        status, result = self.request("/api/record", {"action":"start"})
        self.assertEqual(status, 409, result)
        self.assertEqual(self.state()["work"]["files"], running["work"]["files"])

    def test_record_buttons_require_power_awake_and_unlocked(self):
        self.post("/api/sim", reset=True, pause=True)
        for action in ("start", "stop", "mark"):
            status, result = self.request("/api/record", {"action":action})
            self.assertEqual((status, result["rc"]), (409, -1))
        self.boot()
        self.key(7)  # Short POWER sleeps the screen, without switching off.
        self.assertFalse(self.state()["dev"]["power"]["screen"])
        status, result = self.request("/api/record", {"action":"start"})
        self.assertEqual((status, result["rc"]), (409, -1))
        self.key(7)  # Wake only.
        self.post("/api/key", k=2, down=True)
        self.post("/api/key", k=3, down=True)
        self.advance(2000)
        self.post("/api/key", k=2, down=False)
        self.post("/api/key", k=3, down=False)
        self.assertTrue(self.state()["dev"]["power"]["locked"])
        for action in ("start", "stop", "mark"):
            status, result = self.request("/api/record", {"action":action})
            self.assertEqual((status, result["rc"]), (409, -2))
        self.assertEqual(self.state()["dev"]["rec"]["sid"], 0)

    def test_record_buttons_preserve_drafts_and_pending_dialogs(self):
        self.post("/api/tx/edit", ch=0, index=0)
        draft = self.state()["dev"]["ui"]
        for action in ("start", "stop", "mark"):
            status, result = self.request("/api/record", {"action":action})
            self.assertEqual((status, result["rc"]), (409, -4))
            self.assertEqual(self.state()["dev"]["ui"], draft)
        self.post("/api/touch", kind=1, a=0)
        self.navigate("live")
        self.post("/api/ui", action="filter_open")
        draft = self.state()["dev"]["ui"]
        status, result = self.request("/api/record", {"action":"start"})
        self.assertEqual((status, result["rc"]), (409, -4))
        self.assertEqual(self.state()["dev"]["ui"], draft)
        self.post("/api/touch", kind=1, a=0)
        self.connect_host()
        self.post("/api/host", cmd="tx_send", a=0, b=0)
        dialog = self.state()["dev"]["ui"]["dlg"]
        self.assertTrue(dialog.get("kind"))
        for action in ("start", "stop", "mark"):
            status, result = self.request("/api/record", {"action":action})
            self.assertEqual((status, result["rc"]), (409, -3))
            self.assertEqual(self.state()["dev"]["ui"]["dlg"], dialog)
        self.assertEqual(self.state()["dev"]["rec"]["sid"], 0)

    def test_record_buttons_validation_and_generation_are_atomic(self):
        before = self.state()
        for command in ({"action":"bad"}, {"action":True}, {"action":0},
                        {"action":"start", "generation":True},
                        {"action":"start", "generation":-1}):
            status, result = self.request("/api/record", command)
            self.assertEqual(status, 400, result)
        generation = before["sim"]["generation"]
        self.post("/api/sim", reset=True, pause=True)
        self.boot()
        state = self.state()
        status, _ = self.request("/api/record", {"action":"start", "generation":generation})
        self.assertEqual(status, 409)
        self.assertEqual(self.state()["dev"]["rec"], state["dev"]["rec"])
        self.assertEqual(self.state()["work"]["files"], state["work"]["files"])

    def test_record_buttons_reject_missing_sd(self):
        self.post("/api/env", cmd="sd_remove")
        self.advance(20)
        before = self.state()
        status, result = self.request("/api/record", {"action":"start"})
        self.assertEqual((status, result["rc"]), (409, -9))
        self.assertEqual(self.state()["dev"]["rec"], before["dev"]["rec"])

    def test_database_examples_are_real_parseable_files(self):
        for kind in ("dbc", "dbf"):
            status, data = self.request(f"/api/database/example?format={kind}")
            self.assertEqual(status, 200)
            self.assertIn(b"BO_ 291" if kind == "dbc" else b"[START_MSG]", data)
            catalog = self.post("/api/database/import", name=f"download.{kind}", content=base64.b64encode(data).decode())
            self.assertGreaterEqual(len(catalog["frames"]), 2)
        status, _ = self.request("/api/database/example?format=txt")
        self.assertEqual(status, 400)

    def test_database_import_preview_and_apply_are_channel_specific_and_never_send(self):
        before = self.state()
        catalog = self.import_database()
        self.assertEqual(self.state()["work"]["tx"], before["work"]["tx"])
        self.assertEqual(len(catalog["frames"]), 3)
        self.assertTrue(catalog["warnings"])
        self.apply_database(catalog, ch=1, start=1)
        state = self.state()
        self.assertEqual(state["work"]["tx"][0], before["work"]["tx"][0])
        self.assertEqual(state["work"]["tx"][1][0], before["work"]["tx"][1][0])
        rows = state["work"]["tx"][1][1:]
        self.assertEqual([(row["id"], row["len"], row["flags"]) for row in rows],
                         [(0x123,8,0), (0x18FF50E5,8,1), (0x3A1,64,6)])
        self.assertEqual(rows[-1]["data"].split(), ["00"] * 64)
        self.assertFalse(any(row["active"] for entries in state["work"]["tx"] for row in entries))
        self.assertFalse(state["dev"]["tx_active"])
        self.assertFalse(state["dev"]["ui"]["dlg"].get("kind"))
        self.assertEqual([ch["state"] for ch in state["dev"]["ch"]], ["只听", "只听"])

    def test_database_busmaster_extended_id_and_manual_edit_after_import(self):
        catalog = self.import_database("dbf")
        self.apply_database(catalog, ch=0)
        entries = self.state()["work"]["tx"][0]
        self.assertEqual((entries[1]["id"], entries[1]["flags"]), (0x18FF50E5,1))
        self.post("/api/tx-entry", **self.frame(id="456", len=2, data="AB CD", revision=entries[0]["revision"]))
        edited = self.state()["work"]["tx"][0][0]
        self.assertEqual((edited["id"], edited["data"]), (0x456,"AB CD"))
        self.assertFalse(edited["active"])

    def test_database_import_errors_preserve_existing_catalog_and_entries(self):
        catalog = self.import_database()
        before = self.state()["work"]["tx"]
        for request in ({"name":"bad.dbc", "content":"not-base64"},
                        {"name":"bad.dbc", "content":base64.b64encode(b"BO_ broken frame\n").decode()},
                        {"name":"table.dbf", "content":base64.b64encode(b"\x03\x20\0\0").decode()},
                        {"name":"wrong.txt", "content":base64.b64encode(b"BO_ 1 Frame: 8 Node\n").decode()}):
            status, result = self.request("/api/database/import", request)
            self.assertEqual(status, 422, result)
            self.assertEqual(self.state()["work"]["tx"], before)
        self.apply_database(catalog, count=1)  # Failed attempts did not evict it.

    def test_database_apply_stale_second_slot_is_atomic(self):
        catalog = self.import_database()
        self.post("/api/tx-entry", **self.frame(ch=1, index=2, id="222", data="01 02 03 04"))
        before = self.state()["work"]["tx"]
        status, result = self.request("/api/database/apply", {"database":catalog["database"], "ch":1,
                                     "start_index":1, "frames":[frame["key"] for frame in catalog["frames"][:2]], "revisions":[0,0]})
        self.assertEqual(status, 409, result)
        self.assertEqual(self.state()["work"]["tx"], before)

    def test_database_apply_selection_validation_is_atomic(self):
        catalog = self.import_database()
        key = catalog["frames"][0]["key"]
        valid = {"database":catalog["database"], "ch":0,"start_index":0,"frames":[key],"revisions":[0]}
        before = self.state()["work"]["tx"]
        for change in ({"frames":[]}, {"frames":[key,key],"revisions":[0,0]},
                       {"frames":[key,"unknown"],"revisions":[0,0]}, {"revisions":[True]},
                       {"start_index":4}, {"ch":2}, {"frames":[key,key,key,key,key],"revisions":[0]*5}):
            status, result = self.request("/api/database/apply", {**valid, **change})
            self.assertEqual(status, 400, result)
            self.assertEqual(self.state()["work"]["tx"], before)

    def test_database_apply_cannot_bypass_draft_dialog_or_lock(self):
        catalog = self.import_database()
        request = {"database":catalog["database"], "ch":0,"start_index":0,
                   "frames":[catalog["frames"][0]["key"]],"revisions":[0]}
        before = self.state()["work"]["tx"]
        self.post("/api/tx/edit",ch=0,index=0)
        status, _ = self.request("/api/database/apply", request)
        self.assertEqual(status, 409)
        self.assertEqual(self.state()["work"]["tx"], before)
        self.post("/api/touch",kind=1,a=0)
        self.connect_host()
        self.post("/api/host",cmd="tx_send",a=0,b=0)
        dialog = self.state()["dev"]["ui"]["dlg"]
        status, _ = self.request("/api/database/apply", request)
        self.assertEqual(status, 409)
        self.assertEqual(self.state()["dev"]["ui"]["dlg"], dialog)
        self.post("/api/touch",kind=2,a=1)
        self.post("/api/key", k=2, down=True)
        self.post("/api/key", k=3, down=True)
        self.advance(2000)
        self.post("/api/key", k=2, down=False)
        self.post("/api/key", k=3, down=False)
        self.assertTrue(self.state()["dev"]["power"]["locked"])
        status, _ = self.request("/api/database/apply", request)
        self.assertEqual(status, 409)
        self.assertEqual(self.state()["work"]["tx"], before)

    def test_database_preview_expires_and_reset_cannot_target_new_slots(self):
        old = self.import_database()
        for _ in range(2): self.import_database()
        request = {"database":old["database"], "ch":0,"start_index":0,
                   "frames":[old["frames"][0]["key"]],"revisions":[0]}
        status, _ = self.request("/api/database/apply", request)
        self.assertEqual(status, 409)
        current = self.import_database()
        generation = self.state()["sim"]["generation"]
        self.post("/api/sim",reset=True,pause=True)
        self.boot()
        before = self.state()["work"]["tx"]
        status, _ = self.request("/api/database/apply", {**request,"database":current["database"],"generation":generation})
        self.assertEqual(status,409)
        self.assertEqual(self.state()["work"]["tx"],before)

    def test_review_navigation_cannot_bypass_key_lock(self):
        self.navigate("live")
        self.post("/api/key", k=2, down=True)
        self.post("/api/key", k=3, down=True)
        self.advance(2000)
        self.post("/api/key", k=2, down=False)
        self.post("/api/key", k=3, down=False)
        self.assertTrue(self.state()["dev"]["power"]["locked"])
        before = self.state()["dev"]["ui"]
        for command in ({"action":"navigate", "page":"tx"},
                        {"action":"live_pause", "paused":True},
                        {"action":"filter_open"}):
            status, _ = self.request("/api/ui", command)
            self.assertEqual(status, 409)
            self.assertEqual(self.state()["dev"]["ui"], before)

    def test_full_fd_detail_is_stable_and_returns_to_live_context(self):
        for channel in (0, 1):
            self.post("/api/env", cmd="bus_fps", a=channel, b=0)
        data = " ".join(f"{byte:02X}" for byte in range(64))
        self.post("/api/tx-entry", **self.frame(id="18FF50E5", flags=7, len=64, data=data))
        self.connect_host()
        self.post("/api/host", cmd="tx_send", a=0, b=0)
        self.post("/api/touch", kind=2, a=0)
        self.advance(100)
        self.assertFalse(self.state()["dev"]["ui"]["dlg"].get("kind"))
        self.navigate("live")
        live = self.state()["dev"]["ui"]["live"]
        row = next(row for row in live["rows"] if row["id"] == 0x18FF50E5)
        self.post("/api/ui", action="live_detail", index=row["i"])
        ui = self.state()["dev"]["ui"]
        self.assertEqual(ui["page"], "detail")
        detail = ui["detail"]
        self.assertEqual((detail["ch"], detail["id"], detail["len"]), (1, 0x18FF50E5, 64))
        self.assertEqual(detail["flags"] & 7, 7)
        self.assertEqual(detail["data"], list(range(64)))
        self.assertEqual(detail["available_len"], 64)
        self.assertFalse(detail["truncated"])
        self.post("/api/env", cmd="bus_fps", a=0, b=100)
        self.advance(500)
        self.assertEqual(self.state()["dev"]["ui"]["detail"], detail)
        self.post("/api/touch", kind=1, a=0)
        returned = self.state()["dev"]["ui"]
        self.assertEqual(returned["page"], "live")
        self.assertEqual(returned["live"]["view"], live["view"])
        self.assertEqual(returned["live"]["paused"], live["paused"])

    def test_detail_preserves_explicit_pause_and_live_only_controls(self):
        self.navigate("live")
        self.post("/api/ui", action="live_view", view=1)
        self.post("/api/ui", action="live_pause", paused=True)
        before = self.state()["dev"]["ui"]["live"]
        self.assertGreater(len(before["rows"]), 0)
        row = before["rows"][0]
        self.post("/api/ui", action="live_detail", index=row["i"])
        detail = self.state()["dev"]["ui"]
        for command in ({"action":"live_view", "view":0}, {"action":"live_pause", "paused":False},
                        {"action":"live_detail", "index":0}, {"action":"filter_open"}):
            status, _ = self.request("/api/ui", command)
            self.assertEqual(status, 409)
            self.assertEqual(self.state()["dev"]["ui"], detail)
        self.navigate("live")  # Review navigation to LIVE uses the same restore path.
        returned = self.state()["dev"]["ui"]["live"]
        self.assertEqual((returned["view"], returned["paused"], returned["cur"]),
                         (before["view"], True, before["cur"]))

    def test_detail_sequence_survives_live_row_movement(self):
        self.navigate("live")
        before = self.state()["dev"]["ui"]["live"]["rows"][0]
        self.advance(100)
        self.assertNotEqual(self.state()["dev"]["ui"]["live"]["rows"][0]["seq"], before["seq"])
        self.post("/api/ui", action="live_detail", seq=before["seq"])
        detail = self.state()["dev"]["ui"]["detail"]
        self.assertEqual(detail["seq"], before["seq"])
        self.assertEqual(detail["id"], before["id"])
        self.assertEqual(detail["ch"], before["ch"])

    def test_overwritten_detail_sequence_is_rejected(self):
        self.navigate("live")
        sequence = self.state()["dev"]["ui"]["live"]["rows"][0]["seq"]
        self.advance(10000)  # The display keeps only the latest 1024 rows.
        before = self.state()["dev"]["ui"]
        status, _ = self.request("/api/ui", {"action":"live_detail", "seq":sequence})
        self.assertEqual(status, 409)
        self.assertEqual(self.state()["dev"]["ui"], before)

    def test_review_request_from_before_reset_cannot_target_new_state(self):
        generation = self.state()["sim"]["generation"]
        self.post("/api/sim", reset=True, pause=True)
        self.boot()
        before = self.state()
        self.assertNotEqual(before["sim"]["generation"], generation)
        status, _ = self.request("/api/ui", {"action":"navigate", "page":"tx", "generation":generation})
        self.assertEqual(status, 409)
        self.assertEqual(self.state()["dev"]["ui"], before["dev"]["ui"])
        self.post("/api/ui", action="navigate", page="tx", generation=before["sim"]["generation"])
        self.assertEqual(self.state()["dev"]["ui"]["page"], "tx")

    def test_format_cancel_keeps_files_and_recording_idle(self):
        before = self.state()["work"]["files"]
        self.navigate("files")
        format_row = next(index for index, item in enumerate(self.state()["dev"]["ui"]["items"])
                          if item["l"] == "格式化 SD 卡")
        self.post("/api/touch", kind=0, a=format_row)
        self.assertTrue(self.state()["dev"]["ui"]["dlg"].get("kind"))
        self.post("/api/touch", kind=2, a=1)
        self.advance(300)
        state = self.state()
        self.assertEqual(state["work"]["files"], before)
        self.assertEqual(state["dev"]["rec"]["sid"], 0)
        self.assertFalse(state["dev"]["ui"]["dlg"].get("kind"))

    def test_live_control_validation_does_not_mutate(self):
        self.navigate("live")
        before = self.state()["dev"]["ui"]
        for command in ({"action":"live_view", "view":True}, {"action":"live_view", "view":2},
                        {"action":"live_pause", "paused":"false"},
                        {"action":"live_detail", "index":-1}):
            status, _ = self.request("/api/ui", command)
            self.assertEqual(status, 400)
            self.assertEqual(self.state()["dev"]["ui"], before)


if __name__ == "__main__":
    unittest.main(verbosity=2)
