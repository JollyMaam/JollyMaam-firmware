"""Offline restore CLI checks. Never pass a real USB or block-device path."""
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
import zlib


class RestoreCliTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tool = Path(os.environ.get('JOLLYMAAM_TEST_APP', 'build/host/jollymaam')).resolve()
        if not cls.tool.exists():
            raise unittest.SkipTest('Build jollymaam first')

    def packed(self):
        data = bytearray(64)
        data[8:12] = b'KNLT'
        struct.pack_into('<I', data, 24, len(data))
        struct.pack_into('<I', data, len(data)-4, zlib.crc32(data[:-4]) ^ 0xffffffff)
        return bytes(data)

    def run_tool(self, *args):
        return subprocess.run([str(self.tool), *map(str, args)], capture_output=True, text=True, timeout=10)

    def test_stock_images_dat_models_and_corruption(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp)/'stock.bin'
            image = self.packed();p.write_bytes(image)
            self.assertEqual(self.run_tool('8bitdo', 'check-stock', '0x301b', p).returncode, 0)
            dat = struct.pack('<7I', 109, 0x6000, len(image)+8, 0x301b, 0, 0, 0) + image + bytes(8)
            p.write_bytes(dat)
            self.assertEqual(self.run_tool('8bitdo', 'check-stock', '0x301b', p).returncode, 0)
            for bad in (dat[:-1], dat[:12]+struct.pack('<I', 0x301c)+dat[16:], dat[:-1]+b'x',
                        image[:-1], image+b'\0', image[:8]+b'nope'+image[12:]):
                p.write_bytes(bad)
                self.assertNotEqual(self.run_tool('8bitdo', 'check-stock', '0x301b', p).returncode, 0)
            p.write_bytes(image)
            self.assertNotEqual(self.run_tool('8bitdo', 'check-stock', '0x301c', p).returncode, 0)

    def test_restore_requires_confirmation_and_never_overwrites_backup(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp);image = root/'stock.bin';image.write_bytes(self.packed())
            # Existing output refusal happens before opening a device. An alias
            # of the input must also remain byte-exact.
            backup = root/'backup.bin';backup.write_bytes(b'keep me')
            for output in (backup, image):
                result = self.run_tool('8bitdo', 'restore', '--yes', root/'not-a-device', '0x301b', image, output)
                self.assertNotEqual(result.returncode, 0)
            self.assertEqual(backup.read_bytes(), b'keep me');self.assertEqual(image.read_bytes(), self.packed())
            self.assertEqual(self.run_tool('8bitdo', 'restore').returncode, 2)
            self.assertEqual(self.run_tool('stock-loader').returncode, 2)
            self.assertEqual(self.run_tool('jieli', 'restore', '--yes').returncode, 2)
