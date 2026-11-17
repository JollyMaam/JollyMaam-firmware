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


class BluetoothLoader:
    """padhid.Device for the Ultimate 2C Bluetooth's loader: CPU addresses,
    identify answers 4 bytes, an erase reply says 4 bytes, and cmd 3 data is
    decrypted as one stream from the app base, programmed only after an
    erase there with the same total (as seen on the pad)."""
    def __init__(self):
        self.flash = bytearray(b"\xaa" * (boot.BT_END - boot.BT_START))
        self.commands, self.replies, self.stream, self.armed, self.landed = [], [], bytearray(), None, 0

    def drain(self):
        self.replies.clear()

    def write(self, data):
        command, _, n, crc, total, address = struct.unpack_from("<HHHHII", data, 2)
        self.commands.append((command, address, total))
        body, count = b"", 0
        if command == 4 and address == boot.BT_START:
            self.flash[:total] = b"\xff" * total
            self.armed, self.stream = total, bytearray()
            count = 4
        elif command == 3:
            payload = data[18:18 + n]
            assert crc == boot.crc16(payload)
            assert address == boot.BT_START + len(self.stream)
            self.stream += payload
            if total == self.armed:
                self.landed = len(self.stream) // 512 * 512      # a short last block is dropped
        elif command == 5:
            if self.landed:                                      # decrypted once, when read
                self.flash[:self.landed] = boot.pad8cryptdo.transform(bytes(self.stream[:self.landed]))
                self.landed = 0
            body = bytes(self.flash[address - boot.BT_START:address - boot.BT_START + n])
            count = n
        elif command == 8:
            body = b"\x1a\x30\0\0"
            count = 4
        if command != 7:
            reply = b"\0\x05" + struct.pack("<HHHHII", 0, command, count, 0, total, address) + body
            self.replies.append(reply.ljust(64, b"\0"))

    def read(self, timeout):
        return self.replies.pop(0) if self.replies else None

    def close(self):
        pass


class BluetoothTests(unittest.TestCase):
    def setUp(self):
        self.device = BluetoothLoader()
        patcher = patch.object(boot.padhid, "Device", return_value=self.device)
        patcher.start()
        self.addCleanup(patcher.stop)

    def test_cipher_matches_the_device_tested_vector(self):
        # from alphaorderly's transform, which the pad accepted on 2026-10-30
        self.assertEqual(boot.pad8cryptdo.transform(bytes(16), encrypt=True).hex(),
                         "514228f4d87352bcb198f1b5ba69baef")
        data = bytes(range(256)) * 9
        self.assertEqual(boot.pad8cryptdo.transform(boot.pad8cryptdo.transform(data, encrypt=True)), data)

    def test_app_is_erased_streamed_read_back_and_started(self):
        app = bytes(range(251)) * 400 + b"\x01\x02"           # not a multiple of 4 or 46
        boot.flash_bluetooth("hid", app)
        padded = app + b"\xff" * (-len(app) % boot.BT_BLOCK)
        self.assertEqual(bytes(self.device.flash[:len(padded)]), padded)
        writes = [c for c in self.device.commands if c[0] in (3, 4)]
        self.assertEqual(writes[0], (4, boot.BT_START, len(padded)))
        self.assertTrue(all(total == len(padded) for command, _, total in writes[1:]))
        self.assertEqual(self.device.commands[-1][0], 7)

    def test_unprogrammed_stream_is_caught(self):
        original = self.device.write

        def unarmed(data):
            original(data)
            self.device.armed = None                             # acknowledged, nothing programmed
        self.device.write = unarmed
        with boot.Loader("hid") as loader:
            with self.assertRaisesRegex(OSError, "does not read back"):
                loader.write_bluetooth_app(b"\x11" * 4096)

    def test_other_pads_and_oversized_apps_are_not_written(self):
        with self.assertRaisesRegex(ValueError, "bytes"):
            boot.flash_bluetooth("hid", b"\0" * (boot.BT_END - boot.BT_START + 1))
        self.assertFalse([c for c in self.device.commands if c[0] in (3, 4)])
        with patch.object(boot.padhid, "Device", return_value=FakeLoader("hid")):
            with self.assertRaisesRegex(ValueError, "U2C_WIRELESS"):
                boot.flash_bluetooth("hid", b"\0" * 64)
        self.assertEqual(boot.model_pid(b"\x1a\x30\0\0"), "U2C_BLUETOOTH")


class StockFake:
    """padhid.Device for stock firmware's vendor interface."""
    def __init__(self, replies=(), write_error=None):
        self.replies, self.written, self.write_error = list(replies), [], write_error

    def drain(self):
        pass

    def write(self, data):
        if self.write_error:
            raise self.write_error
        self.written.append(bytes(data))

    def read(self, timeout):
        return self.replies.pop(0) if self.replies else None

    def close(self):
        pass


def model_reply(model):
    reply = bytearray(64)
    reply[:2] = b"\x02\x05"
    struct.pack_into("<HHHHII", reply, 2, 0, 8, 2, 0, 2, 0)
    reply[18:20] = model
    return bytes(reply)


class StockTests(unittest.TestCase):
    def test_model_query_and_boot_entry_reports(self):
        fake = StockFake([b"\x01" + bytes(63), model_reply(b"\x1c\x30")])
        with patch.object(boot.padhid, "Device", return_value=fake):
            with boot.Stock("hid") as device:
                self.assertEqual(device.identify(), b"\x1c\x30")
                device.reboot_loader()
        self.assertEqual(fake.written, [b"\x81\x05\x08" + bytes(61), b"\x81\x05\0\x51\0" + bytes(59)])

    def test_unrelated_input_times_out(self):
        with patch.object(boot.padhid, "Device", return_value=StockFake([b"\x01" + bytes(63)])):
            with self.assertRaises(TimeoutError):
                boot.Stock("hid").identify(timeout=0.05)

    def test_boot_entry_write_may_lose_to_the_reset(self):
        import errno
        with patch.object(boot.padhid, "Device", return_value=StockFake(write_error=OSError(errno.ENODEV, "gone"))), \
             patch.object(boot.sys, "platform", "linux"):
            boot.Stock("hid").reboot_loader()
        with patch.object(boot.padhid, "Device", return_value=StockFake(write_error=PermissionError(errno.EACCES, "denied"))), \
             patch.object(boot.sys, "platform", "linux"):
            with self.assertRaises(PermissionError):
                boot.Stock("hid").reboot_loader()

    def test_report_descriptor_requires_output_on_81(self):
        self.assertTrue(boot.has_stock_output(bytes.fromhex("85 81 95 3f 91 02")))
        for descriptor in ("85 81 81 02", "85 01 91 02", "85", "fe"):
            self.assertFalse(boot.has_stock_output(bytes.fromhex(descriptor)))

    def test_shared_usb_pid_is_resolved_by_model(self):
        hids = [{"path": p, "vid": 0x2dc8, "product": 0x310a, "location": loc, "descriptor": bytes.fromhex("85 81 91 02")}
                for p, loc in (("/dev/hidraw1", "1-1"), ("/dev/hidraw2", "1-2"))]
        hids.append({"path": "/dev/hidraw3", "vid": 0x2dc8, "product": 0x310a, "location": "1-2",
                     "descriptor": bytes.fromhex("85 01 81 02")})       # its gamepad interface: not asked
        fakes = [StockFake([model_reply(m)]) for m in (b"\x1b\x30", b"\x1c\x30")]
        with patch.object(boot.padhid, "Device", side_effect=fakes) as opened:
            self.assertEqual(boot.find_stock(hids), [("/dev/hidraw1", "1-1", "U2C_WIRELESS"),
                                                     ("/dev/hidraw2", "1-2", "U2C_RECEIVER")])
        self.assertEqual(opened.call_count, 2)


if __name__ == "__main__":
    unittest.main()
