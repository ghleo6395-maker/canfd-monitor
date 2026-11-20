"""Real HTTP regressions for the device's own menus and hardware keys.

The only setup transport is /api/local (open a screen), /api/touch and /api/key.
No computer timing form or PC TX editor performs the settings under test.
Each class owns a random-port server; never contacts the user's port 8765.
"""
from copy import deepcopy
import re
import unittest

import test_web as transport


class LocalDeviceTests(unittest.TestCase):
    setUpClass = classmethod(transport.WebTests.setUpClass.__func__)
    tearDownClass = classmethod(transport.WebTests.tearDownClass.__func__)
    post, state = transport.WebTests.post, transport.WebTests.state
    boot, advance, key = transport.WebTests.boot, transport.WebTests.advance, transport.WebTests.key
    setUp = transport.WebTests.setUp

    def request(self, path, obj=None, raw=None, headers=None):
        # A regression must fail if a supposedly local journey starts using a
        # computer configuration endpoint to perform its actual setting writes.
        prohibited = {'/api/sequence', '/api/tx-entry', '/api/tx/edit', '/api/record',
                      '/api/record/name', '/api/log/rename', '/api/database/apply'}
        self.assertNotIn(path.split('?', 1)[0], prohibited,
                         'device-local flow used a computer configuration endpoint')
        return transport.WebTests.request(self, path, obj, raw, headers)

    def local(self, kind, ch=0, index=0):
        return self.post('/api/local', kind=kind, ch=ch, index=index,
                         generation=self.state()['sim']['generation'])

    def ui(self):
        return self.state()['dev']['ui']

    def items(self):
        return self.ui().get('items', [])

    def item_index(self, label):
        matches = [(n, item) for n, item in enumerate(self.items())
                   if label == item.get('l') or re.search(label, item.get('l', ''))]
        self.assertEqual(len(matches), 1, (label, matches, self.ui()))
        return matches[0][0]

    def item_value(self, label):
        return self.items()[self.item_index(label)]['v']

    def tap(self, label):
        self.post('/api/touch', kind=0, a=self.item_index(label))

    @staticmethod
    def numeric_value(text, radix=10):
        number = re.search(r'0x([0-9A-Fa-f]+)|([0-9A-Fa-f]+)' if radix == 16 else r'0x([0-9A-Fa-f]+)|([0-9]+)', text)
        if not number:
            raise AssertionError(f'no numeric value in {text!r}')
        return int(number.group(1), 16) if number.group(1) else int(number.group(2), radix)

    def edit_number(self, label, target, radix=10):
        """Edit only by pressing the device keyboard's visible character keys."""
        self.tap(label)
        ui = self.ui()
        self.assertEqual(ui['page'], 'local_input', ui)
        self.tap('清空')
        for character in (f'{target:X}' if radix == 16 else str(target)):
            self.tap(character)
        self.tap('确定')
        self.assertNotEqual(self.ui()['page'], 'local_input')
        self.assertFalse(self.ui()['numeric'])
        self.assertEqual(self.numeric_value(self.item_value(label), radix), target)

    def no_host_or_tx(self):
        state = self.state()
        self.assertFalse(state['dev']['power']['host'])
        self.assertFalse(state['dev']['storage']['usb'])
        self.assertTrue(all(entry['sent'] == 0 for bus in state['work']['tx'] for entry in bus))
        self.assertTrue(all(q['state'] == 0 for q in state['work']['sequence']))

    def enter_first_timing(self):
        index = next(n for n, item in enumerate(self.items())
                     if re.search(r'第\s*1\s*条', item.get('l', '')))
        self.post('/api/touch', kind=0, a=index)
        self.assertEqual(self.ui()['page'], 'txtiming')

    def test_can2_timing_is_edited_and_saved_only_with_device_controls(self):
        before = deepcopy(self.state()['work']['sequence'])
        self.local(0)
        self.assertEqual(self.ui()['page'], 'txset')
        self.tap('发送通道')
        self.assertIn('CAN2', self.item_value('发送通道'))
        self.edit_number('不同 ID 间隔', 17)
        self.enter_first_timing()
        self.edit_number('每轮次数', 10)
        self.edit_number('同 ID 间隔', 7)
        self.edit_number('单 ID 总周期', 120)
        self.assertEqual(self.state()['work']['sequence'], before)
        self.tap('保存本路设置')
        state = self.state()
        self.assertEqual(state['dev']['ui']['page'], 'tx')
        self.assertEqual(state['work']['sequence'][0], before[0])
        saved = state['work']['sequence'][1]
        self.assertEqual(saved['id_gap_ms'], 17)
        self.assertEqual(saved['entries'][0],
                         {'enabled': True, 'repeat_count': 10, 'frame_gap_ms': 7, 'cycle_ms': 120})
        self.assertEqual(saved['entries'][1:], before[1]['entries'][1:])
        self.no_host_or_tx()

    def test_numeric_cancel_and_entry_cancel_leave_saved_configuration_unchanged(self):
        before = deepcopy(self.state()['work']['sequence'])
        self.local(1, ch=1)
        self.tap('每轮次数')
        self.tap('清空')
        self.tap('2')
        self.assertEqual(self.ui()['keyboard']['value'], '2')
        self.key(5)  # BACK discards only the keyboard edit.
        self.assertFalse(self.ui()['numeric'])
        self.assertEqual(self.numeric_value(self.item_value('每轮次数')), 1)
        self.edit_number('每轮次数', 3)
        self.tap('取消本条修改')
        self.assertEqual(self.ui()['page'], 'txset')
        self.assertEqual(self.state()['work']['sequence'], before)
        self.enter_first_timing()
        self.assertEqual(self.numeric_value(self.item_value('每轮次数')), 1)
        self.no_host_or_tx()

    def test_invalid_local_cycle_keeps_draft_and_does_not_partially_save(self):
        before = deepcopy(self.state()['work']['sequence'])
        self.local(1)
        self.edit_number('每轮次数', 10)
        self.edit_number('单 ID 总周期', 250)
        self.edit_number('同 ID 间隔', 20)
        self.tap('单 ID 总周期')
        self.tap('清空')
        for character in '100':
            self.tap(character)
        self.tap('确定')  # needs at least 181ms, keep the invalid keyboard draft.
        self.assertEqual(self.ui()['page'], 'local_input')
        self.assertEqual(self.state()['work']['sequence'], before)
        self.assertTrue(self.ui().get('local_error'), self.ui())
        self.no_host_or_tx()

    def test_local_draft_cannot_be_discarded_by_shortcut_navigation(self):
        self.local(1, ch=1, index=1)
        self.edit_number('每轮次数', 2)
        before = deepcopy(self.ui())
        status, result = self.request('/api/ui', {'action': 'navigate', 'page': 'home'})
        self.assertEqual(status, 409, result)
        self.assertEqual(result['rc'], -4)
        self.assertEqual(self.ui()['page'], before['page'])
        self.assertEqual(self.item_value('每轮次数'),
                         next(item['v'] for item in before['items'] if item['l'] == '每轮次数'))
        status, result = self.request('/api/local', {'kind': 0, 'ch': 0, 'index': 0,
                                                    'generation': self.state()['sim']['generation']})
        self.assertEqual(status, 409, result)
        self.assertEqual(self.ui()['page'], before['page'])
        self.no_host_or_tx()

    def test_local_defaults_restore_times_and_preserve_frames_counts_and_selection(self):
        self.local(1, ch=1)
        self.edit_number('每轮次数', 3)
        self.edit_number('同 ID 间隔', 12)
        self.edit_number('单 ID 总周期', 150)
        self.tap('参与分组循环')  # disable the selected slot
        self.tap('保存本路设置')
        before = deepcopy(self.state()['work'])
        self.local(0, ch=1)
        self.tap('本路时间恢复默认')
        self.assertEqual(self.state()['work'], before)  # only the screen draft changed
        self.tap('保存本路设置')
        after = self.state()['work']
        self.assertEqual(after['tx'], before['tx'])
        self.assertEqual(after['sequence'][0], before['sequence'][0])
        saved = after['sequence'][1]
        self.assertEqual(saved['id_gap_ms'], 10)
        self.assertEqual(saved['entries'][0],
                         {'enabled': False, 'repeat_count': 3, 'frame_gap_ms': 10, 'cycle_ms': 100})
        self.no_host_or_tx()

    def test_both_channels_default_times_can_be_set_from_the_local_overview(self):
        for ch in range(2):
            self.local(1, ch=ch)
            self.edit_number('每轮次数', ch + 2)
            self.edit_number('同 ID 间隔', 20)
            self.edit_number('单 ID 总周期', 250)
            self.tap('保存本路设置')
        before = deepcopy(self.state()['work'])
        self.local(0, ch=1)
        self.tap('两路时间恢复默认')
        after = self.state()['work']
        self.assertEqual(after['tx'], before['tx'])
        for ch in range(2):
            self.assertEqual(after['sequence'][ch]['id_gap_ms'], 10)
            self.assertEqual(after['sequence'][ch]['entries'][0]['repeat_count'], ch + 2)
            self.assertEqual(after['sequence'][ch]['entries'][0]['frame_gap_ms'], 10)
            self.assertEqual(after['sequence'][ch]['entries'][0]['cycle_ms'], 100)
        self.no_host_or_tx()

    def test_local_start_confirmation_and_busy_defaults_do_not_bypass_safety(self):
        self.local(0, ch=1)
        self.tap('开始本路循环')
        self.assertTrue(self.ui()['dlg'].get('kind'))
        self.assertTrue(all(e['sent'] == 0 for bus in self.state()['work']['tx'] for e in bus))
        self.key(5)
        self.assertEqual(self.state()['work']['sequence'][1]['state'], 0)
        self.tap('开始本路循环')
        self.key(4)
        self.advance(10)
        self.assertEqual(self.state()['work']['sequence'][1]['state'], 3)
        before = deepcopy(self.state()['work']['sequence'])
        self.tap('两路时间恢复默认')
        after = self.state()['work']['sequence']
        for ch in range(2):
            self.assertEqual(after[ch]['entries'], before[ch]['entries'])
            self.assertEqual(after[ch]['revision'], before[ch]['revision'])
        self.tap('停止本路发送')
        self.assertEqual(self.state()['work']['sequence'][1]['state'], 0)
        self.assertEqual(self.state()['work']['sequence'][0]['state'], 0)
        self.assertFalse(self.state()['dev']['power']['host'])

    def test_frame_id_and_payload_are_edited_with_local_keyboard(self):
        # Home directly opens the editable send list.
        self.post('/api/touch', kind=0, a=1)
        self.assertEqual(self.ui()['page'], 'tx')
        self.post('/api/touch', kind=0, a=0)
        self.assertEqual(self.ui()['page'], 'txedit')
        self.edit_number('报文 ID', 0x124, radix=16)
        self.edit_number('数据字节值', 0xAB, radix=16)
        self.tap('保存')
        after = self.state()
        self.assertEqual(after['dev']['ui']['page'], 'tx')
        self.assertEqual(after['work']['tx'][0][0]['id'], 0x124)
        self.assertTrue(after['work']['tx'][0][0]['data'].startswith('AB '))
        self.no_host_or_tx()

    def test_can2_database_frame_is_selected_from_the_local_sd_catalogue(self):
        before = deepcopy(self.state()['work']['tx'])
        self.post('/api/touch', kind=0, a=1)  # native Home -> editable list
        self.assertEqual(self.ui()['page'], 'tx')
        self.local(2, ch=1, index=2)
        self.assertEqual(self.ui()['page'], 'local_db')
        self.tap(r'review\.dbc')
        self.assertEqual(self.ui()['page'], 'local_frames')
        self.tap('VehicleStatus')
        self.assertEqual(self.ui()['page'], 'local_frame')
        self.assertIn('123', self.item_value('ID'))
        self.tap('加入')
        after = self.state()['work']['tx']
        self.assertEqual(after[0], before[0])
        self.assertEqual(after[1][:2], before[1][:2])
        self.assertEqual(after[1][3], before[1][3])
        selected = after[1][2]
        self.assertEqual((selected['id'], selected['flags'], selected['len'], selected['period']),
                         (0x123, 0, 8, 0))
        self.assertEqual(selected['data'], ' '.join(['00'] * 8))
        self.assertEqual(selected['revision'], before[1][2]['revision'] + 1)
        for _ in range(3):
            self.key(5)
        self.assertEqual(self.ui()['page'], 'tx')
        self.assertEqual(self.ui()['cursor'], 6)
        self.no_host_or_tx()

    def test_simulated_sd_database_directory_survives_device_power_cycle(self):
        self.local(2)
        names = [item['l'] for item in self.items() if item['l'].endswith(('.dbc', '.dbf'))]
        self.assertIn('review.dbc', names)
        self.assertIn('review.dbf', names)
        generation = self.state()['sim']['generation']
        self.key(5)
        self.key(7, 2100)
        self.advance(300)
        self.assertEqual(self.state()['dev']['power']['sid'], 0)
        self.boot()
        self.assertEqual(self.state()['sim']['generation'], generation)
        self.local(2)
        after = [item['l'] for item in self.items() if item['l'].endswith(('.dbc', '.dbf'))]
        self.assertEqual(after, names)
        self.no_host_or_tx()

    def test_local_fd_database_frame_preview_can_be_cancelled_without_writing(self):
        before = deepcopy(self.state()['work']['tx'])
        self.local(2, ch=0, index=1)
        self.tap(r'review\.dbc')
        self.tap('FdPayload')
        self.assertEqual(self.ui()['page'], 'local_frame')
        self.assertIn('64', self.item_value('长度'))
        self.key(5)
        self.assertEqual(self.ui()['page'], 'local_frames')
        self.key(5)
        self.assertEqual(self.ui()['page'], 'local_db')
        self.key(5)
        self.assertNotIn(self.ui()['page'], ('local_db', 'local_frames', 'local_frame'))
        self.assertEqual(self.state()['work']['tx'], before)
        self.no_host_or_tx()

    def local_keyboard_text(self, text):
        self.tap('清空')
        for label in text:
            self.tap(label)

    def test_record_prefix_uses_device_keyboard_and_records_two_new_sessions(self):
        self.local(3)
        self.assertEqual(self.ui()['page'], 'local_name')
        self.local_keyboard_text(['路试', 'A'])
        self.assertEqual(self.ui()['keyboard']['value'], '路试A')
        # A hardware arrow followed by OK inserts the focused on-screen key.
        previous_cursor = self.ui()['cursor']
        self.key(3)
        focused = self.ui()
        self.assertNotEqual(focused['cursor'], previous_cursor)
        key = focused['keyboard']['keys'][focused['cursor']]
        self.assertTrue(key['value'])
        self.key(4)
        expected = '路试A' + key['value']
        self.assertEqual(self.ui()['keyboard']['value'], expected)
        self.tap('保存')
        self.assertEqual(self.state()['work']['rec_name'], expected)
        self.assertNotEqual(self.ui()['page'], 'local_name')
        generation = self.state()['sim']['generation']
        self.key(6)  # local REC key starts a new session
        self.advance(200)
        first = deepcopy(self.state()['work']['files'][-1])
        self.assertTrue(first['name'].startswith(expected + '_'))
        self.assertTrue(first['writing'])
        self.key(6)  # second short REC stops/closes the current session
        self.advance(300)
        self.assertEqual(self.state()['dev']['rec']['sid'], 0)
        transport.finish_record_save(self, '保存')
        self.key(6)
        self.advance(200)
        second = self.state()
        self.assertEqual(second['sim']['generation'], generation)
        self.assertEqual(second['dev']['rec']['sid'], 2)
        self.assertNotEqual(second['work']['files'][-1]['token'], first['token'])
        self.assertTrue(second['work']['files'][-1]['name'].startswith(expected + '_'))
        self.assertTrue(any(f['token'] == first['token'] and not f['writing']
                            for f in second['work']['files']))
        self.assertFalse(second['dev']['power']['host'])
        self.assertFalse(second['dev']['storage']['usb'])

    def test_prefix_keyboard_cancel_leaves_name_and_draft_navigation_guard_intact(self):
        before = self.state()['work']['rec_name']
        self.local(3)
        self.local_keyboard_text(['路试', 'A'])
        status, result = self.request('/api/ui', {'action': 'navigate', 'page': 'home'})
        self.assertEqual(status, 409, result)
        self.assertEqual(self.ui()['keyboard']['value'], '路试A')
        self.tap('取消')
        self.assertEqual(self.state()['work']['rec_name'], before)
        self.assertNotEqual(self.ui()['page'], 'local_name')
        self.no_host_or_tx()

    def test_completed_recording_is_renamed_from_local_file_list_and_keyboard(self):
        self.key(6)
        self.advance(200)
        self.key(6)
        self.advance(300)
        transport.finish_record_save(self, '保留默认名')
        recorded = self.state()['work']['files'][-1]
        self.assertFalse(recorded['writing'])
        self.local(5)
        self.assertEqual(self.ui()['page'], 'local_files')
        self.tap(re.escape(recorded['name']))
        self.assertEqual(self.ui()['page'], 'local_rename')
        self.local_keyboard_text(['路试', 'B', '.log'])
        self.tap('保存')
        renamed = next(f for f in self.state()['work']['files'] if f['token'] == recorded['token'])
        self.assertEqual(renamed['name'], '路试B.log')
        self.assertEqual(renamed['sample_frames'], recorded['sample_frames'])
        self.assertEqual(renamed['frames'], recorded['frames'])
        self.no_host_or_tx()

    def test_local_open_checks_generation_parameter_and_power_before_changing_page(self):
        initial = deepcopy(self.ui())
        generation = self.state()['sim']['generation']
        for change in ({'kind': 99}, {'kind': True}, {'ch': 2}, {'index': 24}, {'generation': True}):
            request = {'kind': 0, 'ch': 0, 'index': 0, 'generation': generation, **change}
            status, _ = self.request('/api/local', request)
            self.assertEqual(status, 400, request)
            self.assertEqual(self.ui()['page'], initial['page'])
        self.post('/api/sim', reset=True, pause=True)
        status, result = self.request('/api/local', {'kind': 0, 'ch': 0, 'index': 0, 'generation': generation})
        self.assertEqual(status, 409, result)
        self.assertEqual(self.ui()['page'], 'home')
        status, result = self.request('/api/local', {'kind': 0, 'ch': 0, 'index': 0,
                                                    'generation': self.state()['sim']['generation']})
        self.assertEqual(status, 409, result)
        self.assertEqual(self.ui()['page'], 'home')


if __name__ == '__main__':
    unittest.main(verbosity=2)
