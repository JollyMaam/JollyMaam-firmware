"""Boot selection/framing regressions; all device I/O is simulated."""
import importlib.util
from pathlib import Path
from types import SimpleNamespace
import struct
import unittest
from unittest.mock import Mock, patch

spec = importlib.util.spec_from_file_location("boot", Path(__file__).with_name("8bitdoboot.py"))
boot = importlib.util.module_from_spec(spec)
spec.loader.exec_module(boot)


class BootTests(unittest.TestCase):
    def test_main_image_header(self):
        app = boot.pack(b"\x00" * 8 + b"KNLT" + b"\x00" * 0x30, bytes(6), 0x1000)
        image = boot.main_image(app, "U2C_WIRELESS", 0x20000, 0x26120)
        magic, size, crc, entry, version, at = struct.unpack_from("<4sIIIII", image)
        self.assertEqual((magic, size, entry, version, at), (b"MAIN", len(app), 0x26120, 1, 0x20000))
        self.assertEqual(crc, boot.crc16_ccitt(app))
        self.assertEqual(image[24:40], b"U2C_WIRELESS".ljust(16, b"\0"))
        self.assertEqual(struct.unpack_from("<I", image, 40)[0], boot.crc16_ccitt(image[4:40]))
        self.assertEqual(image[0x120:], app)
        self.assertEqual(boot.crc16_ccitt(b"123456789"), 0x31C3)   # CRC-16/XMODEM check value

    def test_missing_loader_does_not_claim_boot_entry_succeeded(self):
        with patch.object(boot, "wait_loader", return_value=None):
            with self.assertRaisesRegex(SystemExit, "receiver bootloader USB identity"):
                boot.wait_entered_loader("1-2", 1, "U2C_RECEIVER")

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
