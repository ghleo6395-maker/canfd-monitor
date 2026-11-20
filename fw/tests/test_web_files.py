"""Actual HTTP regressions for simulator recording names and file identity.

Uses the existing isolated server harness, never the user's port 8765 service.
"""
import http.client
import unittest
from urllib.parse import quote, unquote, urlencode

import test_web as common


class FileWebTests(unittest.TestCase):
    setUpClass = classmethod(common.WebTests.setUpClass.__func__)
    tearDownClass = classmethod(common.WebTests.tearDownClass.__func__)
    setUp = common.WebTests.setUp
    request = common.WebTests.request
    post = common.WebTests.post
    state = common.WebTests.state
    boot = common.WebTests.boot
    advance = common.WebTests.advance
    key = common.WebTests.key
    navigate = common.WebTests.navigate
    connect_host = common.WebTests.connect_host

    def generation(self):
        return self.state()["sim"]["generation"]

    def import_sample(self, name="sample.log"):
        self.post("/api/log/import", name=name,
                  text="(0.000000) can0 123#AABB\n(1.000000) can1 456##1AABB\n")
        file = self.state()["work"]["files"][-1]
        self.assertEqual(file["name"], name)
        self.assertGreater(file["token"], 0)
        return file

    def rename(self, file, name, generation=None):
        return self.request("/api/log/rename", {
            "index":file["index"], "token":file["token"], "name":name,
            "generation":self.generation() if generation is None else generation})

    def download(self, file, generation=None):
        query = urlencode({"index":file["index"], "token":file["token"],
                           "generation":self.generation() if generation is None else generation})
        connection = http.client.HTTPConnection("127.0.0.1", common.web.PORT, timeout=5)
        connection.request("GET", "/api/log/export?" + query)
        response = connection.getresponse()
        result = (response.status, response.read(), response.getheader("Content-Disposition", ""))
        connection.close()
        return result

    def test_prefix_roundtrip_and_two_recordings_without_power_cycle(self):
        generation = self.generation()
        self.post("/api/record/name", prefix="道路测试样例", generation=generation)
        work = self.state()["work"]
        self.assertEqual(work["rec_name"], "道路测试样例")
        self.assertEqual((work["prefix_max_bytes"],work["name_max_bytes"]), (18,47))
        files = len(work["files"])
        self.post("/api/record", action="start", generation=generation)
        self.advance(300)
        first = self.state()["work"]["files"][-1]
        self.assertTrue(first["writing"])
        self.assertRegex(first["name"], r"^道路测试样例_\d{8}-\d{6}_\d{8}\.log$")
        self.assertEqual(len(first["name"].encode()), 47)
        self.post("/api/record", action="stop", generation=generation)
        self.advance(300)
        first = self.state()["work"]["files"][-1]
        self.assertFalse(first["writing"])
        self.assertEqual(self.download(first)[0], 200)
        common.finish_record_save(self, "保存")
        self.post("/api/record/name", prefix="第二次", generation=generation)
        self.post("/api/record", action="start", generation=generation)
        self.advance(300)
        second = self.state()
        self.assertEqual(second["dev"]["power"]["state"], "运行")
        self.assertEqual(second["sim"]["generation"], generation)
        self.assertEqual(len(second["work"]["files"]), files + 2)
        self.assertNotEqual(second["work"]["files"][-1]["token"], first["token"])
        self.assertTrue(second["work"]["files"][-1]["name"].startswith("第二次_"))
        self.assertEqual(second["work"]["files"][-2], first)

    def test_default_reset_and_lost_rtc_are_explicit(self):
        self.post("/api/record/name", prefix="test", generation=self.generation())
        self.post("/api/record/name", prefix="", generation=self.generation())
        self.assertEqual(self.state()["work"]["rec_name"], "")
        self.post("/api/env", cmd="rtc_lost")
        self.post("/api/record", action="start", generation=self.generation())
        self.advance(300)
        self.assertRegex(self.state()["work"]["files"][-1]["name"], r"^REC_NOTIME_\d{8}\.log$")

    def test_invalid_prefix_is_atomic(self):
        self.post("/api/record/name", prefix="valid", generation=self.generation())
        for prefix in (None,True,42,"road/unsafe","CON","道路测试样例长","1234567890123456789","a\nheader","a\x00b","\ud800"):
            with self.subTest(prefix=repr(prefix)):
                status, result = self.request("/api/record/name", {"prefix":prefix,"generation":self.generation()})
                self.assertIn(status, (400,422), result)
                self.assertEqual(self.state()["work"]["rec_name"], "valid")

    def test_recording_and_closing_block_name_changes(self):
        self.post("/api/record/name", prefix="keep", generation=self.generation())
        self.post("/api/record", action="start", generation=self.generation())
        self.advance(300)
        file = self.state()["work"]["files"][-1]
        for closing in (False,True):
            if closing:
                self.post("/api/record", action="stop", generation=self.generation())
            status, result = self.request("/api/record/name", {"prefix":"blocked","generation":self.generation()})
            self.assertIn(status,(409,422),result)
            self.assertEqual(self.state()["work"]["rec_name"],"keep")
            self.assertIn(self.rename(file,"blocked.log")[0],(409,422))
            self.assertEqual(self.state()["work"]["files"][-1]["name"],file["name"])
        self.advance(300)
        common.finish_record_save(self, "保留默认名")
        self.post("/api/record/name", prefix="allowed", generation=self.generation())
        self.assertEqual(self.rename(file,"closed.log")[0],200)

    def test_unicode_rename_and_download_name_match(self):
        file = self.import_sample()
        before = self.download(file)
        status, result = self.rename(file,"道路_试验.log")
        self.assertEqual(status,200,result)
        current = self.state()["work"]["files"][-1]
        self.assertEqual(current["name"],"道路_试验.log")
        self.assertEqual(current["token"],file["token"])
        status, body, disposition = self.download(current)
        self.assertEqual(status,200)
        self.assertEqual(body,before[1])
        self.assertIn("filename*=UTF-8''",disposition)
        self.assertIn(quote(current["name"],safe=""),disposition)
        self.assertIn(current["name"],unquote(disposition))
        self.assertNotIn("\n",disposition)

    def test_duplicate_or_invalid_names_do_not_overwrite(self):
        first = self.import_sample("road.log")
        second = self.import_sample("other.txt")
        files = self.state()["work"]["files"]
        status, result = self.rename(second,"ROAD.LOG")
        self.assertEqual(status,422,result)
        self.assertEqual(self.state()["work"]["files"],files)
        for name in ("../unsafe.log","LPT1.log","header\r\ninject.log","trace.asc","table.csv","x" * 48 + ".log"):
            status, result = self.rename(second,name)
            self.assertIn(status,(400,422),result)
            self.assertEqual(self.state()["work"]["files"],files)
        status, result = self.request("/api/log/import", {"name":"ROAD.LOG","text":"(0.0) can0 123#AA\n"})
        self.assertEqual(status,422,result)
        self.assertEqual(self.download(first)[0],200)

    def test_fixture_and_in_use_files_are_not_renamed(self):
        fixture = self.state()["work"]["files"][0]
        self.assertIn(self.rename(fixture,"fixture.log")[0],(409,422))
        self.post("/api/record", action="start")
        self.advance(300)
        file = self.state()["work"]["files"][-1]
        self.assertTrue(file["writing"])
        self.assertIn(self.rename(file,"writing.log")[0],(409,422))
        self.assertEqual(self.state()["work"]["files"][-1]["name"],file["name"])

    def test_guard_blocks_draft_sleep_and_locked_metadata_writes(self):
        file = self.import_sample()
        self.post("/api/tx/edit",ch=0,index=0)
        status, _ = self.request("/api/record/name",{"prefix":"draft","generation":self.generation()})
        self.assertEqual(status,409)
        self.assertEqual(self.rename(file,"draft.log")[0],409)
        self.post("/api/touch",kind=1,a=0)
        self.key(7)
        self.assertFalse(self.state()["dev"]["power"]["screen"])
        self.assertEqual(self.rename(file,"sleep.log")[0],409)
        self.key(7)
        self.post("/api/key",k=2,down=True); self.post("/api/key",k=3,down=True)
        self.advance(2000)
        self.post("/api/key",k=2,down=False); self.post("/api/key",k=3,down=False)
        self.assertTrue(self.state()["dev"]["power"]["locked"])
        self.assertEqual(self.rename(file,"locked.log")[0],409)
        self.assertEqual(self.state()["work"]["files"][-1]["name"],file["name"])

    def test_stale_generation_or_token_cannot_target_new_file(self):
        old = self.import_sample()
        generation = self.generation()
        self.post("/api/sim",reset=True,pause=True)
        self.boot()
        current = self.import_sample("new.log")
        self.assertEqual(current["index"],old["index"])
        status, result = self.rename(current,"wrong.log",generation)
        self.assertEqual(status,409,result)
        self.assertEqual(self.download(current,generation)[0],409)
        changed = dict(current,token=current["token"] + 100)
        self.assertIn(self.rename(changed,"wrong.log")[0],(409,422))
        self.assertIn(self.download(changed)[0],(409,422))
        self.assertEqual(self.state()["work"]["files"][-1]["name"],"new.log")


if __name__ == "__main__":
    unittest.main(verbosity=2)
