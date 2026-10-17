"""Device polling tests: no display or hardware needed."""
import queue
import unittest
from unittest.mock import patch

import paddevices


class MonitorTests(unittest.TestCase):
    def setUp(self):
        self.monitor = paddevices.DeviceMonitor(queue.Queue())
        self.identity = b'C2PD\x03\0\0\0' + b'build'.ljust(16, b'\0') + b'unique-id'.ljust(16, b'\0')

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


if __name__ == '__main__':
    unittest.main()
