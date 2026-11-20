"""Strict portable send-plan files and actual on-disk simulated SD persistence."""
import copy
import json
from pathlib import Path
import sys
import tempfile
import unittest
sys.path.insert(0,str(Path(__file__).parents[1]/'sim/web'))
from send_plan_format import (decode_plan,encode_plan,to_flat,from_flat,PlanFormatError,
                              PlanStore,validate_filename,MAX_BYTES)
SINGLE=Path(__file__).parents[1]/'sim/web/examples/pc-template.canplan.json'
DUAL=SINGLE.with_name('dual-channel-example.canplan.json')
class PlanFormatTests(unittest.TestCase):
 def setUp(self):self.single=decode_plan(SINGLE.read_bytes());self.dual=decode_plan(DUAL.read_bytes())
 def entry(self,plan=None):return (plan or self.single)['channels'][0]['entries'][0]
 def test_json_and_c_wire_roundtrip_full_payload_and_dynamic(self):
  for plan in (self.single,self.dual):
   with self.subTest(mask=plan['channel_mask']):
    words=to_flat(plan);self.assertEqual(len(words),259);restored=from_flat(words)
    original=copy.deepcopy(plan);original.pop('description',None);original.pop('project',None)
    self.assertEqual(restored,original);self.assertEqual(decode_plan(encode_plan(plan)),plan)
    self.assertEqual(len(bytes.fromhex(restored['channels'][-1]['entries'][1]['data'])),64)
  self.assertEqual(from_flat(to_flat(self.dual))['channels'][0]['entries'][0]['dynamic']['counter_next'],5)
 def test_duplicate_unknown_missing_and_future_version_rejected(self):
  raw=encode_plan(self.single).decode();self.assertRaises(PlanFormatError,decode_plan,raw.replace('"version": 1','"version": 1, "version": 1'))
  for mutate in (lambda p:p.update(version=2),lambda p:p.update(extra=True),lambda p:p.pop('channels'),lambda p:p.update(version=True)):
   p=copy.deepcopy(self.single);mutate(p);self.assertRaises(PlanFormatError,encode_plan,p)
 def test_malformed_unicode_size_and_numbers(self):
  for raw in (b'',b'\xff',b'{',b'['*1200,b'x'*(MAX_BYTES+1),'{"version":NaN}'):
   with self.subTest(raw=raw[:10]):self.assertRaises(PlanFormatError,decode_plan,raw)
  p=copy.deepcopy(self.single);p['description']='\ud800';self.assertRaises(PlanFormatError,encode_plan,p)
 def test_mask_channels_slots_must_be_complete(self):
  for mutate in (lambda p:p.update(channel_mask=3),lambda p:p['channels'][0].update(channel=1),lambda p:p['channels'][0]['entries'].pop(),lambda p:self.entry(p).update(slot=2)):
   p=copy.deepcopy(self.single);mutate(p);self.assertRaises(PlanFormatError,encode_plan,p)
 def test_invalid_protocol_and_bytes_rejected(self):
  for fields in ({'id':'800'},{'id':'123oops'},{'brs':True},{'fd':True,'rtr':True},{'length':9},{'data':'AA'},{'data':'ZZ 00'},{'enabled':1},{'flags':0}):
   p=copy.deepcopy(self.single);self.entry(p).update(fields);self.assertRaises(PlanFormatError,encode_plan,p)
 def test_fd_dlc_and_rtr_validation(self):
  p=copy.deepcopy(self.single);e=self.entry(p);e.update(fd=True,brs=True,length=64,data=bytes(range(64)).hex());self.assertEqual(decode_plan(encode_plan(p))['channels'][0]['entries'][0]['length'],64)
  e.update(length=10,data=bytes(range(10)).hex());self.assertRaises(PlanFormatError,encode_plan,p)
  e.update(fd=False,brs=False,rtr=True,length=8,data='');self.assertEqual(decode_plan(encode_plan(p))['channels'][0]['entries'][0]['data'],'')
  e['data']='00';self.assertRaises(PlanFormatError,encode_plan,p)
 def test_timing_validation_no_silent_clamping(self):
  for fields in ({'repeat_count':0},{'repeat_count':10001},{'repeat_count':3,'frame_gap_ms':0},{'repeat_count':3,'frame_gap_ms':10,'cycle_ms':20},{'frame_gap_ms':3600001},{'period_ms':65536}):
   p=copy.deepcopy(self.single);self.entry(p).update(fields);self.assertRaises(PlanFormatError,encode_plan,p)
 def test_dynamic_ranges_checksum_and_overlap(self):
  for fields in ({'counter_bits':7},{'counter_bits':4,'counter_max':16},{'counter_bits':4,'counter_max':15,'counter_next':16},{'checksum':'evil'},{'checksum':'xor','checksum_to':8},{'counter_bits':4,'counter_max':15,'checksum':'sum','counter_byte':0,'checksum_byte':0,'checksum_to':7}):
   p=copy.deepcopy(self.single);self.entry(p)['dynamic'].update(fields);self.assertRaises(PlanFormatError,encode_plan,p)
 def test_bad_wire_flags_padding_and_unselected_channel(self):
  good=to_flat(self.single)
  for index,value in ((1,1),(132,128),(137,0x12345678)):
   bad=good.copy();bad[index]=value;self.assertRaises(PlanFormatError,from_flat,bad)
  self.assertRaises(PlanFormatError,from_flat,good[:-1]);bad=good.copy();bad[0]=True;self.assertRaises(PlanFormatError,from_flat,bad)
 def test_project_metadata_unicode_limits_and_wire_independence(self):
  p=copy.deepcopy(self.single);p['project']='项目甲 - 台架01'
  self.assertEqual(decode_plan(encode_plan(p))['project'],p['project'])
  self.assertNotIn('project',from_flat(to_flat(p)))
  p['project']='项'*15+'AB';self.assertEqual(len(p['project'].encode()),47);encode_plan(p)
  for name in ('', ' ', '.', '..', ' A', 'A ', 'A/B', 'A\\B', 'A\nB', 'A\x7fB', 'A\x85B', '项'*16, '\ud800'):
   p['project']=name
   with self.subTest(project=repr(name)):self.assertRaises(PlanFormatError,encode_plan,p)
  p.pop('project');self.assertNotIn('project',decode_plan(encode_plan(p)))
 def test_project_scan_group_and_legacy_default_survive_restart(self):
  with tempfile.TemporaryDirectory() as d:
   p=copy.deepcopy(self.single);p['project']='动力项目'
   legacy=copy.deepcopy(self.dual);legacy.pop('project',None)
   store=PlanStore(d);store.save('project.canplan.json',p);store.save('legacy.canplan.json',legacy)
   files,errors=PlanStore(d).scan();self.assertEqual(errors,[])
   groups={f['name']:f['project'] for f in files};self.assertEqual(groups,{'project.canplan.json':'动力项目','legacy.canplan.json':'未分组'})
   self.assertEqual(PlanStore(d).read('project.canplan.json')['project'],'动力项目')
   before=store.export('project.canplan.json');p['project']='../invalid'
   self.assertRaises(PlanFormatError,store.import_plan,'project.canplan.json',json.dumps(p),overwrite=True)
   self.assertEqual(store.export('project.canplan.json'),before)
 def test_filename_rules(self):
  self.assertEqual(validate_filename('路试A.canplan.json'),'路试A.canplan.json')
  for name in ('../x.canplan.json','x.json','.canplan.json','CON.canplan.json','COM1.canplan.json','A\\x.canplan.json','x\ny.canplan.json','x'*48+'.canplan.json','\ud800.canplan.json'):
   with self.subTest(name=repr(name)):self.assertRaises(PlanFormatError,validate_filename,name)
 def test_filename_matches_board_unicode_rules(self):
  forbidden=list(range(127,160))+list(range(0x200B,0x2010))+list(range(0x2028,0x202F))+list(range(0x2066,0x206A))+[0xFEFF]
  for point in forbidden:
   with self.subTest(point=hex(point)):
    self.assertRaises(PlanFormatError,validate_filename,'x'+chr(point)+'.canplan.json')
  for name in ('CON .canplan.json','LPT9  .canplan.json'):
   self.assertRaises(PlanFormatError,validate_filename,name)
  for name in ('测试🙂.canplan.json','CON_1.canplan.json','COM10.canplan.json','内部 空格.canplan.json','a'*34+'.canplan.json'):
   self.assertEqual(validate_filename(name),name)
 def test_persistence_restart_and_export(self):
  with tempfile.TemporaryDirectory() as d:
   store=PlanStore(d);store.save('路试A.canplan.json',self.dual)
   restarted=PlanStore(d);files,errors=restarted.scan();self.assertEqual(errors,[]);self.assertEqual(files[0]['plan'],self.dual);self.assertEqual(decode_plan(restarted.export(files[0]['name'])),self.dual)
   self.assertEqual(restarted.read(files[0]['name']),self.dual)
 def test_new_name_never_overwrites_and_explicit_replace(self):
  with tempfile.TemporaryDirectory() as d:
   store=PlanStore(d);store.save('a.canplan.json',self.single);before=store.export('a.canplan.json')
   self.assertRaises(PlanFormatError,store.save,'A.canplan.json',self.dual);self.assertRaises(PlanFormatError,store.save,'a.canplan.json',self.dual)
   self.assertEqual(store.export('a.canplan.json'),before);store.save('a.canplan.json',self.dual,overwrite=True);self.assertEqual(store.read('a.canplan.json'),self.dual)
 def test_invalid_import_keeps_disk_bytes_and_no_temp_files(self):
  with tempfile.TemporaryDirectory() as d:
   store=PlanStore(d);store.save('a.canplan.json',self.single);before=store.export('a.canplan.json');bad=copy.deepcopy(self.single);self.entry(bad)['cycle_ms']=0
   self.assertRaises(PlanFormatError,store.import_plan,'new.canplan.json',json.dumps(bad));self.assertEqual(store.export('a.canplan.json'),before);self.assertEqual([p.name for p in Path(d).iterdir()],['a.canplan.json'])
 def test_scan_bad_files_visible_errors_no_good_file_loss(self):
  with tempfile.TemporaryDirectory() as d:
   store=PlanStore(d);store.save('a.canplan.json',self.single);(Path(d)/'bad.canplan.json').write_text('{}');files,errors=store.scan();self.assertEqual([f['name'] for f in files],['a.canplan.json']);self.assertEqual(len(errors),1)
 def test_oversized_json_integer_preserves_saved_plan_and_catalog(self):
  # CPython's integer digit guard raises ValueError, not JSONDecodeError.
  raw=b'{"version":'+b'9'*5000+b'}'
  with tempfile.TemporaryDirectory() as d:
   store=PlanStore(d);store.save('keep.canplan.json',self.single);before=store.export('keep.canplan.json')
   self.assertRaises(PlanFormatError,store.import_plan,'keep.canplan.json',raw,overwrite=True)
   self.assertEqual(store.export('keep.canplan.json'),before)
   (Path(d)/'bad.canplan.json').write_bytes(raw)
   files,errors=store.scan()
   self.assertEqual([f['name'] for f in files],['keep.canplan.json']);self.assertEqual(len(errors),1)
   self.assertIn('bad.canplan.json',errors[0]);self.assertEqual(store.export('keep.canplan.json'),before)
 def test_symlink_is_not_read_or_overwritten(self):
  with tempfile.TemporaryDirectory() as d,tempfile.TemporaryDirectory() as outside:
   secret=Path(outside)/'outside.canplan.json';secret.write_bytes(encode_plan(self.dual));store=PlanStore(d);(Path(d)/'alias.canplan.json').symlink_to(secret)
   self.assertRaises(PlanFormatError,store.read,'alias.canplan.json');self.assertRaises(PlanFormatError,store.save,'alias.canplan.json',self.single,overwrite=True);self.assertEqual(decode_plan(secret.read_bytes()),self.dual)
 def test_capacity_rejected_without_silent_drop(self):
  with tempfile.TemporaryDirectory() as d:
   store=PlanStore(d)
   for n in range(16):store.save(f'p{n}.canplan.json',self.single)
   self.assertRaises(PlanFormatError,store.save,'extra.canplan.json',self.single);self.assertEqual(len(store.scan()[0]),16)
   (Path(d)/'extra.canplan.json').write_bytes(encode_plan(self.single));self.assertRaises(PlanFormatError,store.scan)
if __name__=='__main__':unittest.main(verbosity=2)
