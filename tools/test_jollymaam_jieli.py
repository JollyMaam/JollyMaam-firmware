"""Exercise the integrated Jieli CLI without any USB or block-device access."""
import binascii
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest


class JieliCliTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tool = Path(os.environ.get('JOLLYMAAM_TEST_APP', 'build/host/jollymaam')).resolve()
        if not cls.tool.exists():
            raise unittest.SkipTest('Build jollymaam first')

    def fixture(self, root):
        boot = bytearray(20)
        struct.pack_into('<HHI', boot, 0, 1, 4, 0)
        struct.pack_into('<I', boot, 8, 16)
        struct.pack_into('<H', boot, 14, binascii.crc_hqx(boot[:14], 0))
        boot[16:] = b'boot'
        (root/'uboot.boot').write_bytes(boot)
        (root/'app.bin').write_bytes(b'\xa5'*32)
        (root/'main.bin').write_bytes(b'\x5a'*64)
        (root/'isd_config.ini').write_text('[EXTRA_CFG_PARAM]\nPID=TEST;\nENTRY=0x1e00120;\n'
                                         '[JLNEWFW]\nUBOOT=uboot.boot;\nFILES=app.bin;\n')

    def run_tool(self, root, *args):
        empty = root/'no-devices';empty.mkdir(exist_ok=True)
        return subprocess.run([str(self.tool), *map(str, args)], cwd=root,
                              env={**os.environ, 'DEV_ROOT': str(empty)},
                              capture_output=True, text=True, timeout=10)

    def test_pack_unpack_verify_and_main(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp);self.fixture(root)
            r = self.run_tool(root, 'pack', '-c', 'isd_config.ini', '-o', 'firmware.img',
                              '--main', '0x1000:main.bin', '--main-out', 'update.bin')
            self.assertEqual(r.returncode, 0, r.stderr)
            original = (root/'firmware.img').read_bytes()
            update = (root/'update.bin').read_bytes()
            self.assertEqual(update[:4], b'MAIN');self.assertEqual(update[0x120:], b'\x5a'*64)
            for command in ('list', 'verify'):
                r = self.run_tool(root, command, 'firmware.img')
                self.assertEqual(r.returncode, 0, r.stderr)
                self.assertIn('pid=TEST', r.stdout)
            r = self.run_tool(root, 'unpack', 'firmware.img', 'unpacked')
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual((root/'unpacked/_main.bin').read_bytes(), b'\x5a'*64)
            self.assertIn('jollymaam pack', (root/'unpacked/isd_config.ini').read_text())
            r = self.run_tool(root, 'pack', '-c', 'unpacked/isd_config.ini', '-o', 'rebuilt.img')
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual((root/'rebuilt.img').read_bytes(), original)
            (root/'broken.img').write_bytes(original[:31])
            self.assertNotEqual(self.run_tool(root, 'verify', 'broken.img').returncode, 0)

    def test_device_commands_fail_preflight_and_help_lists_full_workflow(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp);self.fixture(root)
            r = self.run_tool(root, 'flashpack', '--pid', 'WRONG', '-c', 'isd_config.ini')
            self.assertNotEqual(r.returncode, 0)
            self.assertIn('image PID does not match', r.stderr)
            self.assertNotIn('ROM loader', r.stderr)
            for command in ('backup', 'loader', 'flash', 'flashpack'):
                self.assertNotEqual(self.run_tool(root, command).returncode, 0)
            r = self.run_tool(root, 'help-jieli')
            self.assertEqual(r.returncode, 0)
            for command in ('pack', 'flashpack', 'backup', 'loader', 'run', 'scan-jieli'):
                self.assertIn('jollymaam '+command, r.stdout)
