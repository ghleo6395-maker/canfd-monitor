"""Isolated real-HTTP regressions for ID groups; no connection to port 8765.

Reuse the existing transport/boot helpers without re-running inherited tests.
The clock is manually stepped, and on-disk export is an in-memory SD sample.
"""
from copy import deepcopy
from pathlib import Path
import re
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "sim/web"))
import test_web as transport


class SequenceTests(unittest.TestCase):
    setUpClass = classmethod(transport.WebTests.setUpClass.__func__)
    tearDownClass = classmethod(transport.WebTests.tearDownClass.__func__)
    request, post, state = transport.WebTests.request, transport.WebTests.post, transport.WebTests.state
    boot, advance, key = transport.WebTests.boot, transport.WebTests.advance, transport.WebTests.key
    navigate, connect_host = transport.WebTests.navigate, transport.WebTests.connect_host
    begin_recording, stop_recording = transport.WebTests.begin_recording, transport.WebTests.stop_recording

    def setUp(self):
        transport.WebTests.setUp(self)
        for ch in range(2):
            self.post("/api/env", cmd="bus_fps", a=ch, b=0)
            for index in range(4):
                self.post("/api/tx-entry", ch=ch, index=index, id=f"{0x100+index:X}",
                          flags=0, len=1, data=f"{0xA0+index:X}", period=20,
                          revision=self.state()["work"]["tx"][ch][index]["revision"])

    def sequence(self, ch=0):
        return self.state()["work"]["sequence"][ch]

    def seq(self, action, ch=0, **replace):
        state = self.state()
        return self.post("/api/sequence", action=action, ch=ch,
                         revision=state["work"]["sequence"][ch]["revision"],
                         generation=state["sim"]["generation"], **replace)

    def configuration(self, first=(3,10,50), second=None, id_gap=7):
        rows = [{"enabled":False,"repeat_count":1,"frame_gap_ms":10,"cycle_ms":100}
                for _ in range(4)]
        rows[0] = dict(zip(("repeat_count","frame_gap_ms","cycle_ms"),first), enabled=True)
        if second:
            rows[1] = dict(zip(("repeat_count","frame_gap_ms","cycle_ms"),second), enabled=True)
        return {"entries":rows,"id_gap_ms":id_gap}

    def start_confirmed(self, ch):
        reply = self.seq("start", ch)
        self.assertIn(reply["rc"], (0,1))
        if reply["rc"] == 1:
            self.assertTrue(self.state()["dev"]["ui"]["dlg"].get("kind"))
            self.post("/api/touch", kind=2, a=0)
        self.advance(1)
        self.assertEqual(self.sequence(ch)["state"], 3)

    def defaults_all(self, revisions=None):
        state = self.state()
        return self.request("/api/sequence", {
            "action":"defaults_all", "generation":state["sim"]["generation"],
            "revisions":revisions if revisions is not None else
            [row["revision"] for row in state["work"]["sequence"]]})

    def test_group_timing_is_present_in_actual_recorded_tx_samples(self):
        self.begin_recording()
        recording_index = self.state()["work"]["files"][-1]["index"]
        self.seq("configure", **self.configuration(second=(2,5,20)))
        self.start_confirmed(0)
        for _ in range(149): self.advance(1)
        self.assertEqual(self.sequence()["sent"], 10)
        self.assertEqual(self.sequence()["missed"], 0)
        self.seq("stop")
        self.stop_recording()
        status, text = self.request(f"/api/log/export?index={recording_index}")
        self.assertEqual(status,200,text)
        frames = re.findall(rb"\(([0-9.]+)\) can0 (100|101)#", text)
        self.assertEqual(len(frames),10,text)
        first = float(frames[0][0])
        times = [round((float(time)-first)*1000) for time,_ in frames]
        self.assertEqual(times,[0,10,20,57,62,84,94,104,141,146])
        self.assertEqual([ident for _,ident in frames],
                         [b"100"]*3+[b"101"]*2+[b"100"]*3+[b"101"]*2)

    def test_select_can2_and_physical_ok_never_sends_on_can1(self):
        self.navigate("tx")
        self.post("/api/tx/select",ch=1,index=2,generation=self.state()["sim"]["generation"])
        self.post("/api/tx/scope",mask=2,generation=self.state()["sim"]["generation"])
        before=self.state()
        self.assertEqual(before["dev"]["ui"]["cursor"],6)
        self.assertTrue(all(row["sent"]==0 for bus in before["work"]["tx"] for row in bus))
        self.key(4)
        pending=self.state()
        self.assertTrue(pending["dev"]["ui"]["dlg"].get("kind"))
        self.assertEqual(pending["dev"]["ch"][0]["state"],"只听")
        self.assertTrue(all(row["sent"]==0 for bus in pending["work"]["tx"] for row in bus))
        self.post("/api/touch",kind=2,a=0);self.advance(10)
        after=self.state()
        self.assertEqual(after["work"]["tx"][1][2]["sent"],1)
        self.assertTrue(all(row["sent"]==0 for row in after["work"]["tx"][0]))
        self.assertEqual(after["dev"]["ch"][0]["state"],"只听")

    def test_cancelled_confirmation_does_not_send_probe_or_resume(self):
        self.seq("configure",**self.configuration())
        self.assertEqual(self.seq("start")["rc"],1)
        self.advance(60)
        state=self.state()
        self.assertEqual(self.sequence()["sent"],0)
        self.assertEqual(state["dev"]["ch"][0]["state"],"只听")
        self.assertTrue(all(row["sent"]==0 for row in state["work"]["tx"][0]))
        self.post("/api/touch",kind=2,a=1)
        self.advance(200)
        self.assertEqual(self.sequence()["state"],0)
        self.assertEqual(self.sequence()["sent"],0)
        self.assertEqual(self.sequence()["stop_reason"],"已取消启动")

    def test_stale_configuration_and_edit_during_confirmation_are_atomic(self):
        stale=self.sequence()["revision"]
        self.seq("configure",**self.configuration())
        before=self.state()["work"]
        status,result=self.request("/api/sequence",{"action":"start","ch":0,"revision":stale})
        self.assertEqual(status,409,result)
        self.assertEqual(self.state()["work"],before)
        self.seq("start")
        before=self.state()["work"]
        status,result=self.request("/api/sequence",{
            "action":"configure","ch":0,"revision":self.sequence()["revision"],
            **self.configuration(first=(4,10,80))})
        self.assertEqual(status,409,result)
        self.assertEqual(self.state()["work"],before)
        status,_=self.request("/api/tx-entry",{
            "ch":0,"index":0,"id":"222","flags":0,"len":1,"data":"BB","period":20,
            "revision":before["tx"][0][0]["revision"]})
        self.assertEqual(status,422)
        self.assertEqual(self.state()["work"],before)

    def test_stop_one_channel_preserves_other_channel_group_loop(self):
        for ch in range(2):
            self.seq("configure",ch,**self.configuration(first=(2,10,30)))
            self.start_confirmed(ch)
        self.advance(40)
        self.post("/api/tx/control",action="stop",ch=0,index=0)
        before=self.state()["work"]["sequence"]
        self.advance(100)
        after=self.state()["work"]["sequence"]
        self.assertEqual(after[0]["state"],0)
        self.assertEqual(after[0]["sent"],before[0]["sent"])
        self.assertEqual(after[1]["state"],3)
        self.assertGreater(after[1]["sent"],before[1]["sent"])

    def test_stop_one_channel_preserves_other_channel_legacy_periodic(self):
        for ch in range(2):
            self.seq("configure",ch,**self.configuration(first=(1,0,30)))
            self.start_confirmed(ch)
        self.post("/api/tx/control",action="stop",ch=1,index=0)
        self.connect_host()
        self.post("/api/host",cmd="cyc_on",a=1,b=0)
        self.advance(40)
        before=self.state()["work"]["tx"][1][0]
        self.assertTrue(before["active"])
        self.post("/api/tx/control",action="stop",ch=0,index=0)
        self.advance(50)
        after=self.state()["work"]["tx"][1][0]
        self.assertTrue(after["active"])
        self.assertGreater(after["sent"],before["sent"])

    def test_default_all_changes_only_times_and_is_atomic_on_stale_revision(self):
        self.seq("configure",0,**self.configuration(first=(3,11,120),id_gap=22))
        self.seq("configure",1,**self.configuration(first=(5,12,333),id_gap=44))
        before=self.state()["work"]
        status,result=self.defaults_all()
        self.assertEqual(status,200,result)
        after=self.state()["work"]
        self.assertEqual(before["tx"],after["tx"])
        for ch in range(2):
            self.assertEqual(after["sequence"][ch]["id_gap_ms"],10)
            self.assertEqual(after["sequence"][ch]["revision"],before["sequence"][ch]["revision"]+1)
            for old,new in zip(before["sequence"][ch]["entries"],after["sequence"][ch]["entries"]):
                self.assertEqual((old["enabled"],old["repeat_count"]),
                                 (new["enabled"],new["repeat_count"]))
                self.assertEqual(new["frame_gap_ms"],10)
                self.assertEqual(new["cycle_ms"],100)
        status,result=self.defaults_all([before["sequence"][0]["revision"],
                                         after["sequence"][1]["revision"]])
        self.assertEqual(status,409,result)
        self.assertEqual(self.state()["work"],after)

    def test_default_all_rejects_both_when_one_channel_is_active(self):
        self.seq("configure",0,**self.configuration(first=(3,11,120),id_gap=22))
        self.seq("configure",1,**self.configuration(first=(5,12,333),id_gap=44))
        self.start_confirmed(1)
        before=self.state()["work"]
        status,result=self.defaults_all()
        self.assertEqual(status,409,result)
        self.assertEqual(self.state()["work"],before)

    def test_invalid_configuration_has_no_partial_rows_or_other_channel_changes(self):
        valid={"action":"configure","ch":0,"revision":self.sequence()["revision"],
               **self.configuration()}
        before=self.state()["work"]
        invalid=[]
        item=deepcopy(valid);item["entries"][0]["enabled"]=1;invalid.append((item,400))
        item=deepcopy(valid);item["entries"][0]["repeat_count"]=True;invalid.append((item,400))
        item=deepcopy(valid);item["entries"][0]["repeat_count"]=10001;invalid.append((item,400))
        item=deepcopy(valid);item["entries"][3]={"enabled":True,"repeat_count":3,"frame_gap_ms":10,"cycle_ms":20};invalid.append((item,409))
        item=deepcopy(valid);item["entries"].pop();invalid.append((item,400))
        item=deepcopy(valid);item["id_gap_ms"]=True;invalid.append((item,400))
        item=deepcopy(valid);item["revision"]=True;invalid.append((item,400))
        item=deepcopy(valid);item["ch"]=2;invalid.append((item,400))
        for payload,expected in invalid:
            with self.subTest(payload=payload):
                status,result=self.request("/api/sequence",payload)
                self.assertEqual(status,expected,result)
                self.assertEqual(self.state()["work"],before)

    def test_pre_reset_generation_cannot_start_replacement_state(self):
        self.seq("configure",**self.configuration())
        before=self.state()
        request={"action":"start","ch":0,"revision":before["work"]["sequence"][0]["revision"],
                 "generation":before["sim"]["generation"]}
        self.post("/api/sim",reset=True,pause=True)
        self.boot()
        after=self.state()["work"]
        status,result=self.request("/api/sequence",request)
        self.assertEqual(status,409,result)
        self.assertEqual(self.state()["work"],after)
        self.assertFalse(self.state()["dev"]["ui"]["dlg"].get("kind"))

    def capture_closed_session(self):
        """Start, receive real simulated frames and close via visible UI API."""
        self.post("/api/record",action="start")
        self.advance(300)
        running=self.state()
        self.assertEqual(running["dev"]["rec"]["sid"],2)
        recording=running["work"]["files"][-1]
        self.assertTrue(recording["writing"])
        self.assertGreater(recording["frames"],0)
        self.post("/api/record",action="stop")
        self.advance(300)
        closed=self.state()
        self.assertEqual(closed["dev"]["rec"]["sid"],0)
        self.assertFalse(closed["work"]["files"][-1]["writing"])
        transport.finish_record_save(self, "保存")
        status,text=self.request(f"/api/log/export?index={recording['index']}")
        self.assertEqual(status,200,text)
        self.assertGreater(len(text),0)
        return closed["dev"]["rec"]["session"],recording,text

    def test_six_record_sessions_start_stop_without_power_cycle_or_sample_loss(self):
        # The old four-sample budget made a fifth recording look as though a
        # reboot was required. Keep every earlier sample while opening six.
        for ch in range(2): self.post("/api/env",cmd="bus_fps",a=ch,b=100)
        before=self.state()
        captures=[self.capture_closed_session() for _ in range(6)]
        after=self.state()
        self.assertEqual(after["sim"]["generation"],before["sim"]["generation"])
        self.assertEqual(after["dev"]["power"]["state"],"运行")
        self.assertEqual(len(after["work"]["files"]),len(before["work"]["files"])+6)
        self.assertEqual(len({file["name"] for _,file,_ in captures}),6)
        sessions=[session for session,_,_ in captures]
        self.assertEqual(sessions,list(range(sessions[0],sessions[0]+6)))
        for _,file,original in captures:
            status,text=self.request(f"/api/log/export?index={file['index']}")
            self.assertEqual(status,200,text)
            self.assertEqual(text,original)

    def test_record_sample_budget_fails_explicitly_without_overwriting_old_samples(self):
        for ch in range(2): self.post("/api/env",cmd="bus_fps",a=ch,b=100)
        budget=self.state()["work"]["limits"]["files"]
        self.assertGreaterEqual(budget,6)
        captures=[self.capture_closed_session() for _ in range(budget)]
        before=self.state()["work"]["files"]
        status,result=self.request("/api/record",{"action":"start"})
        self.assertEqual((status,result.get("rc")),(409,-10),result)
        self.advance(300)
        after=self.state()
        self.assertEqual(after["dev"]["rec"]["sid"],0)
        self.assertEqual(after["work"]["files"],before)
        message=result["error"]
        self.assertRegex(message,"样例|空间|容量|上限|已满")
        for _,file,original in captures:
            status,text=self.request(f"/api/log/export?index={file['index']}")
            self.assertEqual(status,200,text)
            self.assertEqual(text,original)


if __name__ == "__main__":
    unittest.main(verbosity=2)
