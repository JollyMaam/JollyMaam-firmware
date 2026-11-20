"""Device polling tests: no display or hardware needed."""
import queue
import unittest
from unittest.mock import MagicMock, patch

import paddevices

import padhid

# No test may reach a real HID device.
padhid.devices = lambda: []
padhid.closed = lambda: []
padhid.write = lambda path, data: None


class BluetoothTests(unittest.TestCase):
    def test_quiet_without_bleak(self):
        monitor = paddevices.DeviceMonitor(queue.Queue())
        with patch.object(paddevices.padble, "scan", side_effect=SystemExit("padtool: Bluetooth needs bleak")):
            monitor.scan_bluetooth()
        self.assertEqual(monitor.manual_paths, frozenset())
        self.assertTrue(monitor.events.empty())

    def test_a_pad_found_is_connected_and_looked_at(self):
        class Device:
            address, name = "AD:21:85:83:5A:C1", "Cyclone 2 Config"
        monitor = paddevices.DeviceMonitor(queue.Queue())
        with patch.object(paddevices.padble, "scan", return_value=[Device()]), \
             patch.object(paddevices.padble, "link") as link:
            monitor.scan_bluetooth()
        link.assert_called_once()
        self.assertEqual(monitor.manual_paths, {"ble:AD:21:85:83:5A:C1"})


class MonitorTests(unittest.TestCase):
    def setUp(self):
        self.monitor = paddevices.DeviceMonitor(queue.Queue())
        self.hids = []                   # what padhid.devices() finds
        hids = patch.object(paddevices.padhid, 'devices', side_effect=lambda: self.hids)
        hids.start()
        self.addCleanup(hids.stop)
        self.identity = b'C2PD\x03\0\0\0' + b'build'.ljust(16, b'\0') + b'unique-id'.ljust(16, b'\0') + b'GS_C2_ADC_DEVICE'

    def test_only_new_or_changed_devices_load_settings(self):
        device = ('path', 'BR23 UDISK', self.identity)
        with patch.object(paddevices.pad, 'device_paths', return_value=['path']), \
             patch.object(paddevices.pad, 'discover_devices', return_value=[device]), \
             patch.object(paddevices, 'read_device', return_value={'identity': self.identity}) as read:
            first = self.monitor.poll_once()
            self.assertEqual(len(first[2]), 1)
            self.monitor.known = first[1]
            self.assertIsNone(self.monitor.poll_once())
            read.assert_called_once_with(*device)
            changed = self.identity[:8] + b'new-build'.ljust(16, b'\0') + self.identity[24:]
            with patch.object(paddevices.pad, 'discover_devices', return_value=[('path', 'BR23 UDISK', changed)]):
                self.assertEqual(len(self.monitor.poll_once()[2]), 1)
            self.assertEqual(read.call_count, 2)

    def test_operations_pause_polling(self):
        self.monitor.busy.set()
        with patch.object(paddevices.pad, 'discover_devices') as discover:
            self.assertIsNone(self.monitor.poll_once())
            discover.assert_not_called()

    def test_manual_paths_are_polled_and_disconnect_does_not_read_settings(self):
        self.monitor.manual_paths = frozenset(['manual'])
        self.monitor.known = {'manual': self.identity}
        with patch.object(paddevices.pad, 'device_paths', return_value=['auto']), \
             patch.object(paddevices.pad, 'discover_devices', return_value=[]) as discover, \
             patch.object(paddevices, 'read_device') as read:
            snapshot = self.monitor.poll_once()
            self.assertEqual(snapshot[1], {})
            discover.assert_called_once_with(include_loaders=True, paths=['auto', 'manual'])
            read.assert_not_called()


    def test_light_poll_asks_each_disk_once_and_keeps_loaders(self):
        tool, disk, loader = '/dev/hidraw1', '/dev/sg1', '/dev/sg2'
        seen = []

        def discover(include_loaders, paths):
            seen.append(paths)
            return [(loader, 'BR23 UBOOT1.00', None)] if loader in paths else []
        with patch.object(paddevices.sys, 'platform', 'linux'), \
             patch.object(paddevices.os.path, 'exists', return_value=True), \
             patch.object(paddevices, 'tool_paths', return_value=[tool]), \
             patch.object(paddevices.pad, 'device_paths', return_value=[tool, disk, loader]), \
             patch.object(paddevices.pad, 'discover_devices', side_effect=discover), \
             patch.object(paddevices, 'read_device', return_value={'identity': None}), \
             patch.object(paddevices, 'stock_records', return_value=[]):
            first = self.monitor.poll_once(full=False)
            self.monitor.known = first[1]
            self.assertIsNone(self.monitor.poll_once(full=False))
            self.assertEqual(seen, [[tool, disk, loader], [tool]])
            self.assertEqual(first[1], {loader: None})
            self.monitor.poll_once(full=True)
            self.assertEqual(seen[-1], [tool, disk, loader])

    def test_settings_change_counter_is_reported_when_it_moves(self):
        device = ('path', 'Cyclone 2', self.identity)
        self.monitor.known = {'path': self.identity}
        counters = iter([('idle', 5), ('idle', 5), ('idle', 6)])
        with patch.object(paddevices, 'tool_paths', return_value=['path']), \
             patch.object(paddevices.pad, 'device_paths', return_value=['path']), \
             patch.object(paddevices.pad, 'discover_devices', return_value=[device]), \
             patch.object(paddevices.pad, 'status_info', side_effect=lambda path: next(counters)), \
             patch.object(paddevices, 'stock_records', return_value=[]):
            self.assertEqual(self.monitor.poll_once(full=False)[4], {'path': ('idle', 5)})
            self.assertIsNone(self.monitor.poll_once(full=False))
            self.assertEqual(self.monitor.poll_once(full=False)[4], {'path': ('idle', 6)})

    def test_8bitdo_boot_loader_is_asked_its_model_once(self):
        self.hids = [{'path': '/dev/hidraw7', 'vid': 0x2dc8, 'product': 0x3208, 'location': '5-2.4'}]
        loader = MagicMock()
        loader.__enter__.return_value.identify.return_value = b'\x1b\x30'
        with patch.object(paddevices.pad, 'device_paths', return_value=[]), \
             patch.object(paddevices.pad, 'discover_devices', return_value=[]), \
             patch.object(paddevices, 'stock_records', return_value=[]), \
             patch.object(paddevices.pad8bitdo, 'Loader', return_value=loader) as opened:
            first = self.monitor.poll_once()
            record, = first[2]
            self.assertEqual((record['board'], record['product_id'], record['location']), ('boot', 'U2C_WIRELESS', '5-2.4'))
            self.monitor.known = first[1]
            self.assertIsNone(self.monitor.poll_once())
            opened.assert_called_once_with('/dev/hidraw7')
            self.hids = []
            self.assertEqual(self.monitor.poll_once()[1], {})


if __name__ == '__main__':
    unittest.main()