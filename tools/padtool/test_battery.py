"""Battery monitoring reads status, follows one identity and tolerates reconnects."""
import csv
from datetime import datetime
import io
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

import battery


def identity(uid=b'1' * 16, product=b'U2C_WIRELESS', board=0, updater=0):
    return b'C2PD\x02\0' + bytes((board, updater)) + bytes(16) + uid + product.ljust(16, b'\0')


class BatteryTests(unittest.TestCase):
    def test_status_and_reconnect_follow_the_same_pad(self):
        monitor = battery.Monitor('U2C_WIRELESS')
        status = struct.pack('<BBBBHxxIBBH', 0, 255, 1, 0, 3475, 123456, 0, 0, 0)
        with patch.object(battery, 'tool_paths', return_value=['old']), \
             patch.object(battery.pad, 'discover_devices', return_value=[('old', 'pad', identity())]), \
             patch.object(battery.pad, 'cmd', return_value=status) as command:
            reading = monitor.sample()
        self.assertEqual(reading, dict(battery_mv=3475, usb_powered=1, uptime_s='123.456', event='sample', device='old'))
        command.assert_called_once_with('old', 0x01, length=16)
        with patch.object(battery.pad, 'cmd', side_effect=OSError('unplugged')):
            self.assertTrue(monitor.sample()['event'].startswith('disconnected:'))
        self.assertIsNone(monitor.path)
        with patch.object(battery, 'tool_paths', return_value=['other', 'new']), \
             patch.object(battery.pad, 'discover_devices', return_value=[('other', '', identity(b'2'*16)), ('new', '', identity())]):
            self.assertTrue(monitor.connect())
        self.assertEqual(monitor.path, 'new')

    def test_ambiguous_and_non_running_devices(self):
        monitor = battery.Monitor('U2C_WIRELESS')
        with patch.object(battery, 'tool_paths', return_value=[]), \
             patch.object(battery.pad, 'discover_devices', return_value=[('a', '', identity()), ('b', '', identity(b'2'*16))]):
            with self.assertRaisesRegex(ValueError, 'Multiple'):
                monitor.connect()
        with patch.object(battery, 'tool_paths', return_value=[]), \
             patch.object(battery.pad, 'discover_devices', return_value=[('a', '', identity(updater=1)), ('b', '', identity(board=1)), ('c', '', identity(product=b'OTHER'))]):
            self.assertEqual(monitor.sample(), dict(event='disconnected', device=''))

    def test_short_status_is_retried_and_zero_is_preserved(self):
        monitor = battery.Monitor('U2C_WIRELESS')
        monitor.path, monitor.uid = 'pad', '31'*16
        with patch.object(battery.pad, 'cmd', return_value=b''):
            self.assertIn('Short STATUS', monitor.sample()['event'])
        monitor.path = 'pad'
        with patch.object(battery.pad, 'cmd', return_value=bytes(16)):
            self.assertEqual(monitor.sample()['battery_mv'], 0)

    def test_cli_appends_one_header_and_flushes_timestamped_rows(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'battery.csv'
            for _ in range(2):
                with patch('sys.argv', ['battery.py', '--samples', '1', '--output', str(output)]), \
                     patch('sys.stdout', new_callable=io.StringIO) as terminal, \
                     patch.object(battery.Monitor, 'sample', return_value=dict(battery_mv=3500, usb_powered=1, uptime_s='2.000', event='sample', device='pad')):
                    battery.main()
                    self.assertIn('3500', terminal.getvalue())
            with output.open() as stream:
                rows = list(csv.DictReader(stream))
            self.assertEqual(len(rows), 2)
            self.assertEqual(rows[0]['battery_mv'], '3500')
            self.assertEqual(datetime.fromisoformat(rows[0]['timestamp_utc']).utcoffset().total_seconds(), 0)
