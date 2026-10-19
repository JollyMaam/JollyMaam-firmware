"""Live console reads, device identity checks and Tk window lifecycle."""
import queue
import struct
import time
import tkinter as tk
import unittest
from unittest.mock import patch

import padconsole
from paddevices import DeviceMonitor


IDENTITY = b"C2PD\x06\0\0\0" + bytes(32)


class ConsoleReadTests(unittest.TestCase):
    def test_reboot_restarts_log_sequence(self):
        status = bytes(8) + (100).to_bytes(4, "little") + bytes(4)
        with patch.object(padconsole.pad, "discover_devices", return_value=iter([("new-path", "BR23", IDENTITY)])), \
             patch.object(padconsole.pad, "cmd", return_value=status), \
             patch.object(padconsole.pad, "read_log", return_value=(1, "boot\n")) as read:
            result = padconsole.read_console("new-path", IDENTITY, 50, 10000)
        read.assert_called_once_with("new-path", 1)
        self.assertEqual(result, (1, "boot\n", 100, True, 1))

    def test_reused_path_is_rejected_before_reading_logs(self):
        with patch.object(padconsole.pad, "discover_devices", return_value=iter([("path", "BR23", b"other-device")])), \
             patch.object(padconsole.pad, "cmd") as cmd, \
             patch.object(padconsole.pad, "read_log") as read:
            with self.assertRaises(OSError):
                padconsole.read_console("path", IDENTITY, 1, None)
        cmd.assert_not_called()
        read.assert_not_called()

    def test_log_chunk_validation_and_shared_cli_parser(self):
        with patch.object(padconsole.pad, "cmd", return_value=struct.pack("<II", 7, 3) + b"hi\n"):
            self.assertEqual(padconsole.pad.read_log("path", 3), (7, "hi\n"))
        for data in (b"short", struct.pack("<II", 7, 9) + b"hi\n"):
            with patch.object(padconsole.pad, "cmd", return_value=data):
                with self.assertRaises(ValueError):
                    padconsole.pad.read_log("path", 3)


class LiveConsoleTests(unittest.TestCase):
    def setUp(self):
        self.root = tk.Tk()
        self.root.withdraw()
        self.errors = []
        self.root.report_callback_exception = lambda *args: self.errors.append(args)
        self.monitor = DeviceMonitor(queue.Queue())
        self.path = "first-path"
        self.closed = False
        self.reader = patch.object(padconsole, "read_console", side_effect=self.read)
        self.mock_read = self.reader.start()
        self.console = padconsole.LiveConsole(self.root, self.monitor, IDENTITY, "pad",
                                             lambda identity: self.path, self.did_close)

    def did_close(self):
        self.closed = True

    @staticmethod
    def read(path, identity, seq, uptime):
        return seq, "one\ntwo\n" if seq == 1 else "", 100, False, seq

    def pump_until(self, condition):
        deadline = time.monotonic() + 3
        while not condition() and time.monotonic() < deadline:
            self.root.update()
            time.sleep(.01)
        self.assertTrue(condition())

    def tearDown(self):
        self.console.close()
        self.root.destroy()
        self.reader.stop()
        self.assertEqual(self.errors, [])

    def test_streams_without_duplicates_and_stops_when_closed(self):
        self.pump_until(lambda: self.console.seq == 3)
        self.console.next_read = 0
        self.pump_until(lambda: self.mock_read.call_count >= 2 and not self.console.reading)
        text = self.console.output.get("1.0", "end")
        self.assertEqual(text.count("one\ntwo\n"), 1)
        self.assertEqual(self.mock_read.call_args.args[2], 3)
        self.console.close()
        self.assertTrue(self.closed)
        count = self.mock_read.call_count
        self.root.update()
        self.assertEqual(self.mock_read.call_count, count)

    def test_pauses_for_operations_and_resolves_reconnected_path(self):
        self.monitor.busy.set()
        self.root.update()
        self.mock_read.assert_not_called()
        self.assertIn("Paused", self.console.status.cget("text"))
        self.monitor.busy.clear()
        self.path = None
        self.pump_until(lambda: "disconnected" in self.console.status.cget("text"))
        self.mock_read.assert_not_called()
        self.path = "renumbered-path"
        self.pump_until(lambda: self.console.seq == 3)
        self.assertEqual(self.mock_read.call_args.args[0], "renumbered-path")
