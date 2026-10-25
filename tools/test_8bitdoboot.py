"""Boot selection/framing regressions; all device I/O is simulated."""
import importlib.util
import errno
from pathlib import Path
from types import SimpleNamespace
import struct
import unittest
from unittest.mock import Mock, patch

spec = importlib.util.spec_from_file_location("boot", Path(__file__).with_name("8bitdoboot.py"))
boot = importlib.util.module_from_spec(spec)
spec.loader.exec_module(boot)


class BootTests(unittest.TestCase):
    def test_stock_reports_and_model_response(self):
        device = boot.Stock.__new__(boot.Stock)
        device.fd = 7
        response = bytearray(64)
        response[:2] = b"\x02\x05"
        struct.pack_into("<HHHHII", response, 2, 0, 8, 2, 0, 2, 0)
        response[18:20] = b"\x1c\x30"
        with patch.object(boot.select, "select", side_effect=[([], [], []), ([7], [], [])]), \
             patch.object(boot.os, "read", return_value=response), \
             patch.object(boot.os, "write", return_value=64) as write:
            self.assertEqual(device.identify(), b"\x1c\x30")
            self.assertEqual(write.call_args.args, (7, b"\x81\x05\x08" + bytes(61)))
            device.reboot_loader()
            self.assertEqual(write.call_args.args, (7, b"\x81\x05\0\x51\0" + bytes(59)))

    def test_stock_ignores_unrelated_input(self):
        device = boot.Stock.__new__(boot.Stock)
        device.fd = 7
        with patch.object(boot.select, "select", side_effect=[([], [], []), ([7], [], []), ([], [], [])]), \
             patch.object(boot.os, "read", return_value=b"\x01" + bytes(63)), \
             patch.object(boot.os, "write", return_value=64):
            with self.assertRaises(TimeoutError):
                device.identify()

    def test_boot_write_disconnect_can_be_followed_by_enumeration(self):
        device = boot.Stock.__new__(boot.Stock)
        device.fd = 7
        with patch.object(boot.os, "write", side_effect=OSError(errno.ENODEV, "gone")):
            device.reboot_loader()
        with patch.object(boot.os, "write", side_effect=PermissionError(errno.EACCES, "denied")):
            with self.assertRaises(PermissionError):
                device.reboot_loader()

    def test_missing_loader_does_not_claim_boot_entry_succeeded(self):
        with patch.object(boot, "wait_loader", return_value=None):
            with self.assertRaisesRegex(SystemExit, "receiver bootloader USB identity"):
                boot.wait_entered_loader("1-2", 1, "U2C_RECEIVER")

    def test_report_descriptor_requires_output_on_81(self):
        self.assertTrue(boot.has_stock_output(bytes.fromhex("85 81 95 3f 91 02")))
        for descriptor in ("85 81 81 02", "85 01 91 02", "85", "fe"):
            self.assertFalse(boot.has_stock_output(bytes.fromhex(descriptor)))

    def test_shared_usb_pid_is_resolved_by_model(self):
        nodes = [("/dev/hidraw1", 0x2dc8, 0x310a, "1-1"),
                 ("/dev/hidraw2", 0x2dc8, 0x310a, "1-2")]
        devices = [Mock(identify=Mock(return_value=m)) for m in (b"\x1b\x30", b"\x1c\x30")]
        with patch.object(boot, "hidraw_nodes", return_value=nodes), \
             patch.object(boot.Path, "read_bytes", return_value=bytes.fromhex("85 81 91 02")), \
             patch.object(boot, "Stock", side_effect=devices):
            self.assertEqual(boot.stock("U2C_RECEIVER"), [("/dev/hidraw2", "1-2", "U2C_RECEIVER")])
        for device in devices:
            device.close.assert_called_once()

    def test_existing_receiver_loader_is_not_pad(self):
        device = Mock(identify=Mock(return_value=b"\x1c\x30"))
        with patch.object(boot, "loaders", return_value=[("/dev/hidraw1", "1-1")]) as loaders, \
             patch.object(boot, "Loader", return_value=device):
            self.assertEqual(boot.matching_loaders("U2C_WIRELESS", "1-1"), [])
            loaders.assert_called_once_with("1-1")
        device.close.assert_called_once()

    def test_ambiguous_devices_are_not_rebooted(self):
        with patch.object(boot, "matching_loaders", return_value=[]), \
             patch.object(boot, "ours", return_value=[]), \
             patch.object(boot, "stock", return_value=[("a", "1-1", "U2C_RECEIVER"),
                                                       ("b", "1-2", "U2C_RECEIVER")]), \
             patch.object(boot, "Stock") as stock:
            with self.assertRaises(SystemExit):
                boot.enter_loader("U2C_RECEIVER", None, 1)
            stock.assert_not_called()

    def test_stock_entry_waits_on_same_port_and_model(self):
        device = Mock(identify=Mock(return_value=b"\x1c\x30"))
        with patch.object(boot, "matching_loaders", return_value=[]), \
             patch.object(boot, "ours", return_value=[]), \
             patch.object(boot, "stock", return_value=[("a", "1-2", "U2C_RECEIVER")]), \
             patch.object(boot, "Stock", return_value=device), \
             patch.object(boot, "wait_loader", return_value="loader") as wait:
            self.assertEqual(boot.enter_loader("U2C_RECEIVER", "1-2", 3), "loader")
            device.reboot_loader.assert_called_once()
            device.close.assert_called_once()
            wait.assert_called_once_with("1-2", 3, "U2C_RECEIVER")

    def test_model_change_prevents_reboot(self):
        device = Mock(identify=Mock(return_value=b"\x1b\x30"))
        with patch.object(boot, "matching_loaders", return_value=[]), \
             patch.object(boot, "ours", return_value=[]), \
             patch.object(boot, "stock", return_value=[("a", "1-2", "U2C_RECEIVER")]), \
             patch.object(boot, "Stock", return_value=device):
            with self.assertRaises(OSError):
                boot.enter_loader("U2C_RECEIVER", "1-2", 1)
            device.reboot_loader.assert_not_called()
            device.close.assert_called_once()

    def test_receiver_flash_is_not_enabled(self):
        with patch.object(boot, "enter_loader") as enter:
            with self.assertRaises(SystemExit):
                boot.flash(SimpleNamespace(pid="U2C_RECEIVER"))
            enter.assert_not_called()

    def test_wrong_model_prevents_flash_even_after_selection(self):
        image = boot.pack(b"\0" * 8 + b"KNLT" + bytes(20), bytes(6), 4096)
        args = SimpleNamespace(pid="U2C_WIRELESS", image="image", start=0x6000,
                               end=0x73000, device=None, wait=1)
        device = Mock(identify=Mock(return_value=b"\x1c\x30"))
        with patch.object(boot.Path, "read_bytes", return_value=image), \
             patch.object(boot, "enter_loader", return_value="loader"), \
             patch.object(boot, "Loader", return_value=device):
            with self.assertRaises(OSError):
                boot.flash(args)
            device.write_sector.assert_not_called()
            device.start_app.assert_not_called()
            device.close.assert_called_once()


if __name__ == "__main__":
    unittest.main()
