"""Real HTTP send-plan journeys with an isolated on-disk simulated SD.

Reuse the random-port transport harness. No request contacts port 8765 and no
test reads or writes the user's send-plan directory.
"""
from copy import deepcopy
import http.client
import json
import os
from pathlib import Path
import selectors
import socket
import subprocess
import sys
import tempfile
import unittest
from urllib.parse import urlencode

import test_web as transport
from send_plan_format import PlanStore, decode_plan


class SendPlanWebTests(unittest.TestCase):
    setUpClass = classmethod(transport.WebTests.setUpClass.__func__)
    tearDownClass = classmethod(transport.WebTests.tearDownClass.__func__)
    request, post, state = transport.WebTests.request, transport.WebTests.post, transport.WebTests.state
    boot, advance, key = transport.WebTests.boot, transport.WebTests.advance, transport.WebTests.key

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="canfd-plan-http-")
        self.addCleanup(self.directory.cleanup)
        self.previous_store = transport.web.plan_store
        transport.web.plan_store = PlanStore(self.directory.name)
        self.addCleanup(self.restore_store)
        transport.WebTests.setUp(self)
        for channel in range(2):
            self.post("/api/env", cmd="bus_fps", a=channel, b=0)

    def restore_store(self):
        with transport.web.lock:
            transport.web.plan_store = self.previous_store

    def generation(self):
        return self.state()["sim"]["generation"]

    def ui(self):
        return self.state()["dev"]["ui"]

    def tap(self, label):
        items = self.ui().get("items", [])
        matches = [index for index, item in enumerate(items) if item.get("l") == label]
        self.assertEqual(len(matches), 1, (label, items))
        self.post("/api/touch", kind=0, a=matches[0])

    def open_plan(self, mode=0, channel=0):
        self.post("/api/plan/open", mode=mode, ch=channel, generation=self.generation())

    def files(self):
        return self.state()["work"]["send_plans"]["files"]

    def preview(self, name):
        # File management retains read-only details; the send picker now loads
        # directly into the editable TX list (covered by test_menu_workflow).
        self.post("/api/ui", action="navigate", page="files")
        self.tap(name)
        self.assertEqual(self.ui()["page"], "plan_detail")

    def save_local(self, name, channel=1, both=False):
        """Only the device keyboard and Save button perform the file write."""
        self.open_plan(2 if both else 1, channel)
        self.assertEqual(self.ui()["page"], "local_plan_name")
        self.tap("清空")
        for character in name:
            self.tap(character)
        self.tap("方案后缀")
        expected = name + ".canplan.json"
        self.assertEqual(self.ui()["keyboard"]["value"], expected)
        self.tap("保存")
        self.assertEqual(self.ui()["page"], "send_files")
        status = self.state()["work"]["send_plans"]
        self.assertFalse(status["io_pending"], status)
        self.assertEqual(status["error"], 0, status)
        self.assertTrue((Path(self.directory.name) / expected).is_file())
        return next(file for file in self.files() if file["name"] == expected)

    def download(self, file, generation=None):
        params = {"name": file["name"], "token": file["token"],
                  "generation": self.generation() if generation is None else generation}
        connection = http.client.HTTPConnection("127.0.0.1", transport.web.PORT, timeout=5)
        connection.request("GET", "/api/plan/export?" + urlencode(params))
        response = connection.getresponse()
        result = response.status, response.read(), response.getheader("Content-Disposition", "")
        connection.close()
        return result

    def template(self, kind="single"):
        status, result = self.request("/api/plan/template?example=" + kind)
        self.assertEqual(status, 200, result)
        return result

    def import_plan(self, name="PC.canplan.json", plan=None):
        if plan is None:
            plan = self.template()
        self.post("/api/plan/import", name=name, text=json.dumps(plan), generation=self.generation())
        return next(file for file in self.files() if file["name"] == name)

    @staticmethod
    def configuration(state):
        """Ignore counters/revisions while comparing restored user settings."""
        return {
            "tx": [[{key: entry[key] for key in ("id", "flags", "len", "data", "period")}
                    for entry in channel] for channel in state["work"]["tx"]],
            "sequence": [{key: channel[key] for key in ("entries", "id_gap_ms")}
                         for channel in state["work"]["sequence"]],
        }

    def assert_no_transmission(self):
        state = self.state()
        self.assertTrue(all(entry["sent"] == 0 and not entry["active"]
                            for channel in state["work"]["tx"] for entry in channel), state["work"]["tx"])
        self.assertTrue(all(channel["state"] == 0 for channel in state["work"]["sequence"]))
        self.assertEqual([channel["state"] for channel in state["dev"]["ch"]], ["只听", "只听"])
        self.assertFalse(state["dev"]["ui"]["dlg"].get("kind"))

    def test_local_keyboard_save_disk_reset_and_load_restore_full_can2_screen(self):
        payload = " ".join(f"{value:02X}" for value in range(64))
        self.post("/api/tx-entry", ch=1, index=0, id="18FF50E5", flags=7, len=64,
                  data=payload, period=1234, revision=self.state()["work"]["tx"][1][0]["revision"])
        rows = [{"enabled": index == 0, "repeat_count": 3 + index,
                 "frame_gap_ms": 7 + index, "cycle_ms": 100 + index}
                for index in range(4)]
        self.post("/api/sequence", action="configure", ch=1, entries=rows, id_gap_ms=19,
                  revision=self.state()["work"]["sequence"][1]["revision"], generation=self.generation())
        saved = self.configuration(self.state())
        file = self.save_local("Road", channel=1)
        status, raw, disposition = self.download(file)
        self.assertEqual(status, 200, raw)
        self.assertIn("Road.canplan.json", disposition)
        plan = decode_plan(raw)
        self.assertEqual(plan["channel_mask"], 2)
        self.assertEqual(plan["channels"][0]["entries"][0]["data"], payload)
        self.assert_no_transmission()

        self.post("/api/sim", reset=True, pause=True)
        self.boot()
        self.assertEqual([entry["name"] for entry in self.files()], [file["name"]])
        # Recreate the store as a restarted server would; bytes remain on disk.
        transport.web.plan_store = PlanStore(self.directory.name)
        self.post("/api/plan/refresh", generation=self.generation())
        before = self.configuration(self.state())
        self.assertNotEqual(before["tx"][1], saved["tx"][1])
        self.preview(file["name"])
        self.tap("仅加载到手动发送列表")
        restored = self.configuration(self.state())
        self.assertEqual(restored["tx"][1], saved["tx"][1])
        self.assertEqual(restored["sequence"][1], saved["sequence"][1])
        self.assertEqual(restored["tx"][0], before["tx"][0])
        self.assertEqual(restored["sequence"][0], before["sequence"][0])
        self.assert_no_transmission()

    def test_import_dual_template_load_and_local_resave_preserves_dynamic_initial_values(self):
        original = self.template("dual")
        file = self.import_plan(plan=original)
        self.assert_no_transmission()
        self.preview(file["name"])
        self.tap("仅加载到手动发送列表")
        self.assert_no_transmission()
        restored_file = self.save_local("Both", both=True)
        status, raw, _ = self.download(restored_file)
        self.assertEqual(status, 200, raw)
        expected = deepcopy(original)
        expected.pop("description", None)
        expected["project"] = "本机保存"
        self.assertEqual(decode_plan(raw), decode_plan(json.dumps(expected)))
        self.assert_no_transmission()

    def test_invalid_duplicate_and_path_imports_never_overwrite_or_change_tx(self):
        plan = self.template()
        file = self.import_plan(plan=plan)
        before = self.configuration(self.state())
        original = (Path(self.directory.name) / file["name"]).read_bytes()
        bad = deepcopy(plan)
        bad["channels"][0]["entries"][0]["cycle_ms"] = 0
        cases = [("broken.canplan.json", json.dumps(bad)),
                 ("broken.canplan.json", "{}"),
                 (file["name"], json.dumps(plan)),
                 (file["name"].lower(), json.dumps(plan)),
                 ("../outside.canplan.json", json.dumps(plan)),
                 ("CON.canplan.json", json.dumps(plan))]
        for name, text in cases:
            with self.subTest(name=name, text=text[:20]):
                status, result = self.request("/api/plan/import", {
                    "name": name, "text": text, "generation": self.generation()})
                self.assertEqual(status, 422, result)
                self.assertEqual((Path(self.directory.name) / file["name"]).read_bytes(), original)
                self.assertEqual(self.configuration(self.state()), before)
                self.assertEqual([path.name for path in Path(self.directory.name).iterdir()], [file["name"]])
        self.assert_no_transmission()

    def test_stale_download_token_generation_and_post_generation_are_rejected(self):
        file = self.import_plan()
        generation = self.generation()
        self.assertEqual(self.download(file)[0], 200)
        self.assertEqual(self.download(dict(file, token=file["token"] + 100))[0], 409)
        changed = self.template()
        changed["channels"][0]["entries"][0]["id"] = "201"
        with transport.web.lock:
            transport.web.plan_store.save(file["name"], changed, overwrite=True)
        self.assertEqual(self.download(file)[0], 409)
        current = next(entry for entry in self.files() if entry["name"] == file["name"])
        self.assertNotEqual(current["token"], file["token"])
        self.assertEqual(self.download(current)[0], 200)
        self.post("/api/sim", reset=True, pause=True)
        self.boot()
        self.assertEqual(self.download(current, generation)[0], 409)
        for path, payload in (("/api/plan/open", {"mode": 0, "ch": 0}),
                              ("/api/plan/refresh", {}),
                              ("/api/plan/import", {"name": "stale.canplan.json", "text": json.dumps(changed)})):
            status, result = self.request(path, {**payload, "generation": generation})
            self.assertEqual(status, 409, result)
        self.assertEqual([path.name for path in Path(self.directory.name).iterdir()], [file["name"]])
        self.assert_no_transmission()

    def test_template_routes_and_saved_snapshot_do_not_start_automatically(self):
        for example, mask in (("single", 2), ("dual", 3)):
            self.assertEqual(self.template(example)["channel_mask"], mask)
        self.assertIn("$schema", self.template("schema"))
        self.assertEqual(self.request("/api/plan/template?example=unknown")[0], 400)
        file = self.import_plan()
        before = self.configuration(self.state())
        self.preview(file["name"])
        self.assertEqual(self.configuration(self.state()), before)
        self.assert_no_transmission()
        before = deepcopy(self.ui())
        status, result = self.request("/api/plan/open", {"mode":3, "ch":0, "generation":self.generation()})
        self.assertIn(status, (400,409), result)
        self.assertEqual(self.ui(), before)

    def test_local_once_loop_stop_and_import_during_sending_preserves_running_tx(self):
        plan = self.template()
        for index, entry in enumerate(plan["channels"][0]["entries"]):
            entry["enabled"] = index == 0
        plan["channels"][0]["entries"][0].update(repeat_count=3, frame_gap_ms=7, cycle_ms=40)
        file = self.import_plan(plan=plan)
        self.preview(file["name"])
        self.tap("加载并执行一轮")
        self.assertTrue(self.ui()["dlg"].get("kind"))
        self.assertTrue(all(entry["sent"] == 0 for channel in self.state()["work"]["tx"] for entry in channel))
        self.post("/api/touch", kind=2, a=0)
        self.advance(300)
        state = self.state()
        self.assertEqual(state["work"]["sequence"][1]["state"], 0)
        self.assertEqual(state["work"]["sequence"][1]["rounds"], 1)
        self.assertEqual(state["work"]["tx"][1][0]["sent"], 3)
        self.assertTrue(all(entry["sent"] == 0 for entry in state["work"]["tx"][0]))
        self.tap("加载并循环执行")
        if self.ui()["dlg"].get("kind"):
            self.post("/api/touch", kind=2, a=0)
        self.advance(150)
        running = self.state()
        self.assertEqual(running["work"]["sequence"][1]["state"], 3)
        self.assertGreater(running["work"]["tx"][1][0]["sent"], 3)
        self.import_plan("During.canplan.json", self.template("dual"))
        after_import = self.state()
        self.assertEqual(after_import["work"]["tx"], running["work"]["tx"])
        self.assertEqual(after_import["work"]["sequence"], running["work"]["sequence"])
        self.assertEqual(after_import["dev"]["ui"]["page"], "plan_detail")
        self.advance(100)
        self.assertGreater(self.state()["work"]["tx"][1][0]["sent"], running["work"]["tx"][1][0]["sent"])
        self.tap("停止已加载方案")
        stopped = self.state()["work"]["tx"]
        self.advance(200)
        self.assertEqual(self.state()["work"]["tx"], stopped)
        self.assertEqual(self.state()["work"]["sequence"][1]["state"], 0)

    def assert_reset_reports_directory_error_without_breaking_device(self, message_pattern):
        generation = self.generation()
        self.post("/api/sim", reset=True, pause=True)
        state = self.state()
        self.assertEqual(state["sim"]["generation"], generation + 1)
        self.assertEqual(state["dev"]["power"]["state"], "关机")
        self.assertEqual(state["dev"]["now"], 0)
        plans = state["work"]["send_plans"]
        self.assertEqual(plans["error"], -47, plans)
        self.assertFalse(plans["io_pending"], plans)
        self.assertRegex(plans["message"], message_pattern)
        for path, payload in (("/api/plan/open", {"mode": 0, "ch": 0}),
                              ("/api/plan/refresh", {}),
                              ("/api/plan/import", {"name": "stale.canplan.json", "text": "{}"})):
            status, result = self.request(path, {**payload, "generation": generation})
            self.assertEqual(status, 409, result)
        self.boot()
        self.open_plan()
        self.assertEqual(self.ui()["page"], "send_files")
        self.assertEqual(self.state()["work"]["send_plans"]["error"], -47)

    def test_reset_with_overfull_directory_reports_io_error_and_invalidates_old_page(self):
        plan = self.template()
        # Model an external file copy that bypasses the store's 16-file limit.
        for index in range(17):
            (Path(self.directory.name) / f"external{index}.canplan.json").write_text(json.dumps(plan))
        self.assert_reset_reports_directory_error_without_breaking_device(r"16|超过")
        self.assertEqual(len(list(Path(self.directory.name).glob("*.canplan.json"))), 17)

    def test_reset_skips_oversized_integer_file_and_preserves_good_plan(self):
        saved = self.import_plan("keep.canplan.json")
        saved_path = Path(self.directory.name) / saved["name"]
        before = saved_path.read_bytes()
        (Path(self.directory.name) / "bad.canplan.json").write_bytes(b'{"version":'+b'9'*5000+b'}')
        generation = self.generation()
        self.post("/api/sim",reset=True,pause=True)
        state = self.state()
        self.assertEqual(state["sim"]["generation"],generation+1)
        plans = state["work"]["send_plans"]
        self.assertFalse(plans["io_pending"])
        self.assertEqual([file["name"] for file in plans["files"]],[saved["name"]])
        self.assertIn("bad.canplan.json",plans["message"])
        self.assertEqual(saved_path.read_bytes(),before)
        self.boot()
        self.open_plan()
        self.assertEqual(self.ui()["page"],"send_files")

    def test_reset_after_directory_disappears_reports_io_error_and_device_still_works(self):
        # A user can move/delete the simulated SD directory while the server is
        # open. That must not abort reset halfway or kill the HTTP connection.
        Path(self.directory.name).rmdir()
        self.assert_reset_reports_directory_error_without_breaking_device(r"目录|文件|不存在|No such|Errno 2")

    def test_real_server_startup_with_blocked_directory_serves_device_and_io_error(self):
        blocked = Path(self.directory.name) / "ordinary-file"
        blocked.write_text("This is not a directory.")
        state = self.real_server_startup_state(blocked)
        self.assertEqual(state["work"]["send_plans"]["error"], -47)
        self.assertTrue(state["work"]["send_plans"]["message"])
        self.assertEqual(blocked.read_text(), "This is not a directory.")

    def test_real_server_startup_skips_invalid_integer_file(self):
        good = Path(self.directory.name) / "keep.canplan.json"
        good.write_text(json.dumps(self.template()))
        before = good.read_bytes()
        (Path(self.directory.name) / "bad.canplan.json").write_bytes(b'{"version":'+b'9'*5000+b'}')
        state = self.real_server_startup_state(self.directory.name)
        plans = state["work"]["send_plans"]
        self.assertFalse(plans["io_pending"])
        self.assertEqual([file["name"] for file in plans["files"]],[good.name])
        self.assertIn("bad.canplan.json",plans["message"])
        self.assertEqual(good.read_bytes(),before)

    def real_server_startup_state(self, plan_directory):
        with socket.socket() as reservation:
            reservation.bind(("127.0.0.1", 0))
            port = reservation.getsockname()[1]
        environment = dict(os.environ, PORT=str(port), CANFD_SIM_PLAN_DIR=str(plan_directory),
                           PYTHONUNBUFFERED="1")
        process = subprocess.Popen([sys.executable, "sim/web/server.py"],
                                   cwd=Path(__file__).parents[1], env=environment,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            with selectors.DefaultSelector() as ready:
                ready.register(process.stdout, selectors.EVENT_READ)
                self.assertTrue(ready.select(timeout=5), "isolated simulator did not announce startup")
                announcement = process.stdout.readline()
            self.assertIn(f"http://localhost:{port}", announcement)
            connection = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
            connection.request("GET", "/api/state")
            response = connection.getresponse()
            self.assertEqual(response.status, 200)
            state = json.loads(response.read())
            connection.close()
            self.assertEqual(state["dev"]["power"]["state"], "关机")
            return state
        finally:
            process.terminate()
            try:
                process.communicate(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.communicate(timeout=5)


if __name__ == "__main__":
    unittest.main(verbosity=2)
