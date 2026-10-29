"""The 8BitDo boot loader, simulated: framing, firmware.img, flash order, leaving."""
import struct
import unittest
from unittest.mock import patch

import pad8bitdo as boot


def packed(size, pid=b"U2C_WIRELESS"):
    """A firmware.img: a packed updater of size bytes, then update.bin a sector on."""
    updater = bytearray(b"\x11" * (size - 4))
    updater[8:12] = b"KNLT"
    struct.pack_into("<I", updater, 0x18, size)
    updater += struct.pack("<I", boot.crc32(updater))
    update = b"MAIN" + bytes(20) + pid.ljust(16, b"\0") + b"\x22" * 500
    at = -(-size // boot.SECTOR) * boot.SECTOR
    return bytes(updater) + b"\xff" * (at - size) + update


class FakeLoader:
    """padhid.Device for a boot loader with 512 KiB of flash."""
    def __init__(self, path, model=0x301b):
        self.model = model
        self.flash = bytearray(b"\0" * 0x80000)
        self.erased, self.commands, self.replies = [], [], []

    def drain(self):
        self.replies.clear()

    def write(self, data):
        assert data[0] == 0 and data[1] == 5 and len(data) == 65
        command, _, n, crc, total, address = struct.unpack_from("<HHHHII", data, 2)
        self.commands.append(command)
        body = b""
        if command == 4:
            if address >= 0x6000:
                self.erased.append(address)
                self.flash[address:address + boot.SECTOR] = b"\xff" * boot.SECTOR
        elif command == 3:
            payload = data[18:18 + n]
            assert crc == boot.crc16(payload)
            self.flash[address:address + n] = payload
        elif command == 5:
            body = bytes(self.flash[address:address + n])
        elif command == 8:
            body = struct.pack("<H", self.model)
        if command != 7:
            reply = b"\0\x05" + struct.pack("<HHHHII", 0, command, len(body), 0, total, address) + body
            self.replies.append(reply.ljust(64, b"\0"))

    def read(self, timeout):
        return self.replies.pop(0) if self.replies else None

    def close(self):
        pass


class BootTests(unittest.TestCase):
    def setUp(self):
        self.device = FakeLoader("hid")
        patcher = patch.object(boot.padhid, "Device", return_value=self.device)
        patcher.start()
        self.addCleanup(patcher.stop)

    def test_full_image_names_its_product(self):
        self.assertEqual(boot.image_pid(packed(0x5000)), "U2C_WIRELESS")
        with self.assertRaisesRegex(ValueError, "firmware.img"):
            boot.image_pid(packed(0x5000)[:0x5000])             # the updater alone
        bad = bytearray(packed(0x5000))
        bad[0x100] ^= 1
        with self.assertRaisesRegex(ValueError, "CRC"):
            boot.image_pid(bytes(bad))

    def test_flash_writes_every_sector_header_last_and_starts_it(self):
        image = packed(0x5000)
        order = []
        real = boot.Loader.write_sector
        with patch.object(boot.Loader, "write_sector", lambda self, a, d: (order.append(a), real(self, a, d))):
            boot.flash("hid", image)
        self.assertEqual(bytes(self.device.flash[0x6000:0x6000 + len(image)]), image)
        stop = 0x6000 + len(image) + (-len(image) % boot.SECTOR)
        self.assertEqual(order, list(range(0x7000, stop, boot.SECTOR)) + [0x6000])
        self.assertEqual(self.device.commands[-1], 7)

    def test_sectors_already_there_are_not_written_again(self):
        image = packed(0x5000)
        boot.flash("hid", image)
        self.device.erased.clear()
        changed = bytearray(image)
        changed[-1] ^= 1                                         # in the last sector only
        boot.flash("hid", bytes(changed))
        self.assertEqual(self.device.erased, [0x6000 + (len(image) - 1) // boot.SECTOR * boot.SECTOR])

    def test_wrong_pad_is_not_written(self):
        self.device.model = 0x301c                               # the receiver
        with self.assertRaisesRegex(ValueError, "U2C_RECEIVER"):
            boot.flash("hid", packed(0x5000))
        self.assertEqual(self.device.erased, [])
        self.assertNotIn(7, self.device.commands)

    def test_image_past_the_settings_is_refused(self):
        with boot.Loader("hid") as loader:
            with self.assertRaisesRegex(ValueError, "past"):
                loader.write_image(b"\0" * (boot.END - boot.START + 1))
        self.assertEqual(self.device.erased, [])

    def test_leave_starts_the_app_without_writing(self):
        boot.leave("hid")
        self.assertEqual(self.device.commands, [8, 7])
        self.assertEqual(self.device.erased, [])

    def test_find_asks_each_loader_its_model(self):
        hids = [{"path": "a", "vid": 0x2dc8, "product": 0x3208, "location": "5-2"},
                {"path": "b", "vid": 0x16c0, "product": 0x05df, "location": "5-3"}]
        with patch.object(boot.padhid, "devices", return_value=hids):
            self.assertEqual(boot.find(), [("a", "5-2", "U2C_WIRELESS")])


if __name__ == "__main__":
    unittest.main()
