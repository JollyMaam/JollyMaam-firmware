"""BR23 flashing tests against an in-memory SCSI device; no hardware access."""
import binascii
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

import padflash as flash

# No test may reach a real HID device.
flash.padhid.devices = lambda: []
flash.padhid.write = lambda path, data: None


def image(pid='GS_C2_ADC_DEVICE', key=0xffff, eoffset=None, resource=b''):
    data = bytearray(b'\xff' * 8192)
    header = bytearray(b'\xff' * 32)
    header[16:32] = pid.encode().ljust(16, b'\0')
    header = bytearray(flash.enc(header))
    struct.pack_into('<H', header, 0, binascii.crc_hqx(header[2:], 0))
    data[:32] = flash.enc(header)
    record = bytes([8] * 16 + [8 ^ (0 if key & (1 << j) else 128) for j in range(16)])
    record += struct.pack('<H', binascii.crc_hqx(record, 0))
    if eoffset is not None:
        record += b'EOFFSET\0' + struct.pack('<I', eoffset)
    entry = bytearray(32)
    struct.pack_into('<HII', entry, 2, binascii.crc_hqx(record, 0), 0x200, len(record))
    entry[16:32] = b'isd_config.ini\0'.ljust(16, b'\xff')
    struct.pack_into('<H', entry, 0, binascii.crc_hqx(entry[2:], 0))
    data[32:64] = flash.enc(entry)
    data[0x200:0x200 + len(record)] = record
    def entry(label, offset, size, crc=0xffff, flags=0x82, last=0):
        h = bytearray(b'\xff' * 32)
        struct.pack_into('<HII', h, 2, crc, offset, size)
        h[12] = flags
        struct.pack_into('<H', h, 14, last)
        h[16:32] = label.encode().ljust(16, b'\xff')
        struct.pack_into('<H', h, 0, binascii.crc_hqx(h[2:], 0))
        return bytes(h)
    data[64:96] = flash.enc(entry('app_dir_head', 0x1000, 0xffffffff, flags=0x81))
    data[96:128] = flash.enc(entry('key_mac', 0xff000, 0x1000, flags=0x12, last=0xffff))
    payload = bytes(range(127))
    plain = bytearray(b'\xff' * (0x120 + len(payload)))
    plain[32:64] = entry('app.bin', 0x120, len(payload), flash.crc16(payload), last=1)
    plain[0x120:] = payload
    plain[:32] = entry('app_area_head', 0x1e00120, len(plain), flash.crc16(plain[32:]),
                      flags=0x83, last=0 if resource else 1)
    if resource:
        plain.extend(entry('resources', 32, 32 + len(resource), flash.crc16(resource), flags=0x83, last=1))
        plain.extend(resource)
    data[0x1000:0x1000 + len(plain)] = b''.join(
        flash.enc(plain[at:at + 32], key ^ (at >> 2)) for at in range(0, len(plain), 32))
    data[-4:] = b'test'
    return bytes(data)


class Device:
    def __init__(self):
        self.flash = bytearray(b'\xff' * 0x100000)
        self.flash[:8192] = image()
        self.flash[0x70000:0x70004] = b'keep'
        self.commands = []
        self.ram = bytearray()
        self.corrupt_readback = False
        self.written = False

    def scsi(self, path, cdb, length, timeout_ms=3000, out=None, raw_length=False):
        command = int.from_bytes(cdb[:2], 'big')
        self.commands.append(command)
        if command in (0xfb06, 0xfb04):
            address, size = struct.unpack_from('>IH', cdb, 2)
            assert len(out) == size
            assert struct.unpack_from('<H', cdb, 9)[0] == binascii.crc_hqx(out, 0)
            if command == 0xfb06:
                assert address == 0x12000 + len(self.ram)
                self.ram.extend(out)
            else:
                # NOR writes only clear bits.
                assert all((self.flash[address + i] & b) == b for i, b in enumerate(out))
                self.flash[address:address + size] = out
                self.written = True
            return b''
        if command == 0xfd05:
            address, size = struct.unpack_from('>IH', cdb, 2)
            assert size == length
            data = bytes(self.flash[address:address + size])
            if self.corrupt_readback and self.written and address == 0:
                data = bytes([data[0] ^ 1]) + data[1:]
            return data
        response = bytearray(14)
        if command in (0xfb00, 0xfb01):
            address = int.from_bytes(cdb[2:6], 'big')
            size = 0x10000 if command == 0xfb00 else 4096
            assert address % size == 0
            self.flash[address:address + size] = b'\xff' * size
        elif command == 0xfc0a:
            response[2:5] = bytes([20, 0x40, 0xef])
        elif command == 0xfc14:
            response[:4] = (512).to_bytes(4, 'big')
        elif command == 0xfc09:
            crc = binascii.crc_hqx(b'\xff\xff', 0xffff)
            crc = binascii.crc_hqx(b'\xc3', crc)
            lo = 0xff ^ (crc & 255)
            crc = binascii.crc_hqx(b'\xcf', crc)
            hi = 0xff ^ (crc & 255)
            response[4:6] = bytes([hi, lo])
        elif command not in (0xfb08, 0xfc0c):
            raise AssertionError(f'Unexpected command {command:04x}')
        return bytes(cdb[:2]) + response


class FlashTests(unittest.TestCase):
    def setUp(self):
        self.device = Device()
        self.loader = flash.Loader('fake', lambda _: None)
        self.transport = patch.object(flash, 'scsi', side_effect=self.device.scsi)
        self.transport.start()
        self.addCleanup(self.transport.stop)
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.backup = Path(self.temp.name) / 'backup.bin'
        self.loader.start(bytes(range(256)) * 3)

    def assert_no_flash_mutations(self):
        self.assertFalse({0xfb00, 0xfb01, 0xfb04, 0xfc0c}.intersection(self.device.commands))

    def test_ram_upload_and_chip_info(self):
        self.assertEqual(self.device.ram, bytes(range(256)) * 3)
        self.assertEqual(self.loader.key, 0xffff)
        self.assertEqual(self.loader.flash_size, 0x100000)
        self.assert_no_flash_mutations()

    def test_exact_write_backup_and_reboot_after_verification(self):
        before = bytes(self.device.flash)
        data = bytearray(image())
        data[-4:] = b'next'
        self.loader.flash(bytes(data), self.backup)
        self.assertEqual(self.backup.read_bytes(), before)
        self.assertEqual(self.device.flash[:len(data)], data)
        self.assertEqual(self.device.flash[len(data):], before[len(data):])
        self.assertEqual(self.device.commands[-1], 0xfc0c)
        self.assertLess(self.device.commands.index(0xfd05), self.device.commands.index(0xfb01))

    def test_flash_without_backup_still_verifies_and_reboots(self):
        data = image()
        with patch.object(self.loader, 'backup') as backup:
            self.loader.flash(data)
            backup.assert_not_called()
        self.assertEqual(self.device.flash[:len(data)], data)
        self.assertEqual(self.device.commands[-1], 0xfc0c)
        self.assertIn(0xfd05, self.device.commands)
        self.assertFalse(self.backup.exists())

    def test_block_and_sector_erase_boundaries(self):
        data = image() + b'\xff' * (0x11000 - 8192)
        self.loader.flash(data, self.backup)
        self.assertEqual(self.device.commands.count(0xfb00), 1)
        self.assertEqual(self.device.commands.count(0xfb01), 1)
        self.assertEqual(self.device.flash[:len(data)], data)

    def test_pid_mismatch_and_corrupt_conversion_refuse_to_erase(self):
        corrupt = bytearray(image(key=0x4317))
        corrupt[0x1120] ^= 1
        for data in (image('GS_C2_Dongle'), corrupt):
            with self.assertRaises(ValueError):
                self.loader.flash(data, self.backup)
            self.assert_no_flash_mutations()
        self.assertFalse(self.backup.exists())

    def test_different_key_is_converted_before_backup_erase_and_verification(self):
        self.loader.flash_size = 0x80000
        self.loader.key = 0x4317
        self.device.flash[:8192] = image('GS_C2_Dongle', key=0x4317)
        source = image('GS_C2_Dongle')
        before = bytes(self.device.flash[:0x80000])
        reports = []
        self.loader.report = reports.append
        self.loader.flash(source, self.backup)
        self.assertEqual(self.backup.read_bytes(), before)
        self.assertEqual(self.device.flash[:len(source)], image('GS_C2_Dongle', key=0x4317))
        self.assertEqual(source, image('GS_C2_Dongle'))
        self.assertIn('Converting image key 0xffff to device key 0x4317', reports)
        self.assertEqual(self.device.commands[-1], 0xfc0c)

    def test_progress_reports_each_stage_and_byte_totals(self):
        updates = []
        self.loader.progress = lambda stage, done=0, total=None: updates.append((stage, done, total))
        self.loader.flash(image(), self.backup)
        for stage, size in [('Backing up flash', self.loader.flash_size), ('Erasing flash', 8192),
                            ('Writing firmware', 8192), ('Verifying firmware', 8192)]:
            values = [(done, total) for name, done, total in updates if name == stage]
            self.assertEqual(values[0], (0, size))
            self.assertEqual(values[-1], (size, size))
            self.assertEqual(values, sorted(values))
        self.assertEqual(updates[-1][0], 'Restarting device')

    def test_reused_device_path_is_refused_before_transfer(self):
        self.loader.location = '1-2.1'
        with patch.object(flash, 'usb_location', return_value='1-2.4'), patch.object(flash, 'scsi') as scsi:
            with self.assertRaisesRegex(OSError, 'path was reused'):
                self.loader.read(0, 32)
            scsi.assert_not_called()
        self.assert_no_flash_mutations()

    def test_inspect_loader_reads_actual_product_id_without_flashing(self):
        inquiry = b'\0' * 8 + b'BR23    ' + b'UBOOT1.00'.ljust(16, b' ') + b'\0' * 4
        with patch.object(flash, 'scsi', return_value=inquiry), \
             patch.object(flash.Loader, 'start'), \
             patch.object(flash.Loader, 'pid', return_value='GS_C2_ADC_DEVICE'):
            detected = flash.inspect_loader('selected', b'loader', report=lambda _: None)
            self.assertEqual(detected.path, 'selected')
            self.assertEqual(detected.product_id, 'GS_C2_ADC_DEVICE')
        self.assert_no_flash_mutations()

    def test_failed_backup_prevents_erase(self):
        with self.assertRaises(OSError):
            self.loader.flash(image(), Path(self.temp.name) / 'absent/backup.bin')
        self.assert_no_flash_mutations()

    def test_failed_verification_does_not_reboot(self):
        self.device.corrupt_readback = True
        with self.assertRaisesRegex(OSError, 'readback differs'):
            self.loader.flash(image(), self.backup)
        self.assertTrue(self.backup.exists())
        self.assertNotIn(0xfc0c, self.device.commands)

    def test_short_read_rejected(self):
        with patch.object(flash, 'scsi', return_value=b'bad'):
            with self.assertRaisesRegex(OSError, 'Short loader response'):
                self.loader.read(0, 32)

    def test_ambiguous_devices_refused_before_flash_mutation(self):
        def scsi(path, cdb, length, timeout_ms=3000, out=None, raw_length=False):
            if cdb[0] == 0x12:
                return b'\0' * 8 + b'BR23    ' + b'UBOOT1.00'.ljust(16, b' ') + b'\0' * 4
            return self.device.scsi(path, cdb, length, timeout_ms, out)
        # The start-up emulator expects independent RAM for each device.
        with patch.object(flash, 'device_paths', return_value=['one', 'two']), \
             patch.object(flash, 'scsi', side_effect=scsi), \
             patch.object(flash.Loader, 'start', lambda obj, blob: (setattr(obj, 'flash_size', 0x100000), setattr(obj, 'key', 0xffff))):
            with self.assertRaisesRegex(ValueError, 'Multiple matching'):
                flash.select_loader('GS_C2_ADC_DEVICE', b'loader', wait=0, report=lambda _: None)
        self.assert_no_flash_mutations()

    def test_explicit_loader_path_ignores_other_devices(self):
        inquiry = b'\0' * 8 + b'BR23    ' + b'UBOOT1.00'.ljust(16, b' ') + b'\0' * 4
        with patch.object(flash, 'device_paths', return_value=['one', 'two']), \
             patch.object(flash, 'scsi', return_value=inquiry) as scsi, \
             patch.object(flash.Loader, 'start', lambda obj, blob: (setattr(obj, 'flash_size', 0x100000), setattr(obj, 'key', 0xffff))), \
             patch.object(flash.Loader, 'pid', return_value='GS_C2_ADC_DEVICE'):
            loader = flash.select_loader('GS_C2_ADC_DEVICE', b'loader', wait=0, report=lambda _: None, device_path='two')
            self.assertEqual(loader.path, 'two')
            self.assertEqual([call.args[0] for call in scsi.call_args_list], ['two'])
        self.assert_no_flash_mutations()

    def test_stock_pad_is_sent_its_command_only_for_its_pid(self):
        green = {"path": "hid", "vid": 0x3537, "product": 0x1053, "bcd": 0x0352, "manufacturer": "",
                 "product_name": "", "location": "5-2.3", "descriptor": bytes.fromhex("06f0ff0940a101")}
        writes = []
        with patch.object(flash, 'device_paths', return_value=[]), \
             patch.object(flash.padhid, 'devices', return_value=[green]), \
             patch.object(flash.padhid, 'write', side_effect=lambda p, d: writes.append((p, d))):
            with self.assertRaisesRegex(OSError, 'No ROM loader holding AC695X_TRANS'):
                flash.select_loader('AC695X_TRANS', b'loader', wait=0, report=lambda _: None)
            self.assertEqual(writes, [])
            with self.assertRaisesRegex(OSError, 'No ROM loader holding GS_C2_ADC_DEVICE'):
                flash.select_loader('GS_C2_ADC_DEVICE', b'loader', wait=0, report=lambda _: None)
            self.assertEqual(writes, [('hid', bytes([0x0f, 0x17, 0x55, 0x88]).ljust(64, b'\0'))])

    def test_our_firmware_for_another_pid_is_left_running(self):
        inquiry = b'\0' * 8 + b'BR23    ' + b'AC695X_TRANS'.ljust(16, b' ') + b'\0' * 4
        ident = b'C2PD\x02\0\0\0' + bytes(32) + b'AC695X_TRANS'.ljust(16, b'\0')
        def scsi(path, cdb, length, timeout_ms=3000, out=None, raw_length=False):
            return inquiry if cdb[0] == 0x12 else ident if cdb[0] == 0xf5 else b''
        with patch.object(flash, 'device_paths', return_value=['ours']), \
             patch.object(flash, 'scsi', side_effect=scsi) as sent:
            with self.assertRaises(OSError):
                flash.select_loader('GS_C2_ADC_DEVICE', b'loader', wait=0, report=lambda _: None)
            self.assertNotIn(0xfc, [call.args[1][0] for call in sent.call_args_list])

    def test_explicit_loader_path_refuses_running_firmware(self):
        inquiry = b'\0' * 8 + b'BR23    ' + b'UDISK'.ljust(16, b' ') + b'\0' * 4
        with patch.object(flash, 'device_paths', return_value=['one']), \
             patch.object(flash, 'scsi', return_value=inquiry) as scsi:
            with self.assertRaisesRegex(OSError, 'changed mode'):
                flash.select_loader('GS_C2_ADC_DEVICE', b'loader', wait=0, device_path='one', report=lambda _: None)
            self.assertEqual(scsi.call_count, 1)
            self.assertEqual(scsi.call_args.args[1][0], 0x12)
        self.assert_no_flash_mutations()


class ImageTests(unittest.TestCase):
    def test_rekey_round_trip_preserves_payload_padding_and_input(self):
        for old, new in ((0xffff, 0x4317), (0x4317, 0xffff), (0, 0xa55a)):
            original = image(key=old, resource=b'resource payload' * 3)
            source = bytearray(original)
            converted = flash.rekey_image(source, new)
            self.assertEqual(source, original)
            self.assertEqual(converted, image(key=new, resource=b'resource payload' * 3))
            self.assertEqual(flash.rekey_image(converted, old), original)
            self.assertEqual(flash.image_identity(converted), ('GS_C2_ADC_DEVICE', new))
        self.assertEqual(flash.rekey_image(original, old), original)

    def test_rekey_uses_active_eoffset_copy(self):
        source = image(eoffset=16).ljust(0x10000, b'\xff') + image(key=0x4317)
        converted = flash.rekey_image(source, 0x1234)
        self.assertEqual(converted[:0x10000], source[:0x10000])
        self.assertEqual(converted[0x10000:], image(key=0x1234))
        self.assertEqual(flash.image_identity(converted), ('GS_C2_ADC_DEVICE', 0x1234))

    def test_rekey_rejects_corrupt_and_truncated_areas(self):
        for at in (32, 64, 0x200, 0x1000, 0x1120, 0x11bf):
            data = bytearray(image(resource=b'R' * 50))
            data[at] ^= 1
            with self.subTest(at=at), self.assertRaises(ValueError):
                flash.rekey_image(data, 0x4317)
        with self.assertRaises(ValueError):
            flash.rekey_image(image()[:0x1150], 0x4317)

    def test_rekey_rejects_unsupported_stock_directories_and_check_records(self):
        for offset, label in ((96, b'app_dir_head2'), (0xfe0, b'check')):
            source = bytearray(image())
            h = bytearray(flash.enc(source[96:128]))
            h[16:32] = label.ljust(16, b'\xff')
            struct.pack_into('<H', h, 0, flash.crc16(h[2:]))
            source[offset:offset + 32] = flash.enc(h)
            with self.subTest(label=label), self.assertRaisesRegex(ValueError, 'unsupported'):
                flash.rekey_image(source, 0x4317)

    def test_chip_keys_and_corruption(self):
        for key in (0, 0xffff, 0x4317):
            self.assertEqual(flash.image_identity(image(key=key)), ('GS_C2_ADC_DEVICE', key))
        data = bytearray(image())
        data[0x200] ^= 1
        with self.assertRaisesRegex(ValueError, 'record CRC'):
            flash.image_identity(data)
        with self.assertRaises(ValueError):
            flash.image_identity(b'\xff' * 8192)

    def test_eoffset_selects_active_copy_and_its_key(self):
        data = bytearray(b'\xff' * 0x12000)
        data[:8192] = image(key=0xffff, eoffset=16)
        data[0x10000:] = image(key=0x4317)
        self.assertEqual(flash.image_identity(data), ('GS_C2_ADC_DEVICE', 0x4317))

    def test_ufw_crc_and_extraction(self):
        raw = image()
        header = bytearray(64)
        item = bytearray(80)
        struct.pack_into('<H', item, 4, flash.crc16(raw))
        struct.pack_into('<II', item, 8, 144, len(raw))
        item = flash.enc(item)
        struct.pack_into('<H', header, 2, flash.crc16(item))
        struct.pack_into('<H', header, 8, 1)
        struct.pack_into('<H', header, 0, flash.crc16(header[2:]))
        ufw = bytearray(flash.enc(header) + item + raw)
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'image.ufw'
            path.write_bytes(ufw)
            self.assertEqual(flash.load_image(path), raw)
            self.assertEqual(flash.rekey_image(flash.load_image(path), 0x4317), image(key=0x4317))
            ufw[-1] ^= 1
            path.write_bytes(ufw)
            with self.assertRaisesRegex(ValueError, 'CRC mismatch'):
                flash.load_image(path)


class LoaderResidueTests(unittest.TestCase):
    @unittest.skipUnless(__import__('sys').platform == 'linux', 'Linux SG_IO quirk')
    def test_only_loader_reads_ignore_incorrect_residue(self):
        import ctypes
        import padtransport
        def ioctl(fd, operation, header):
            ctypes.memmove(header.dxferp, b'fc0a', 4)
            header.resid = 4
        with patch.object(padtransport.os, 'open', return_value=9), \
             patch.object(padtransport.os, 'close'), \
             patch.object(padtransport.fcntl, 'ioctl', side_effect=ioctl):
            self.assertEqual(padtransport.scsi('fake', [0xfc, 0x0a], 4), b'')
            self.assertEqual(padtransport.scsi('fake', [0xfc, 0x0a], 4, raw_length=True), b'fc0a')



if __name__ == '__main__':
    unittest.main()
