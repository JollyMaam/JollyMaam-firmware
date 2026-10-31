"""Release bundles preserve executable modes, inputs and reproducible bytes."""
from pathlib import Path
import tempfile
import unittest
import zipfile

from tools.jollymaam_dist import build_desktop, build_sdk


class DistributionTests(unittest.TestCase):
    def test_linux_bundle(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            exe, firmware = root / 'exe', root / 'firmware.zip'
            exe.write_bytes(b'\x7fELFtest')
            with zipfile.ZipFile(firmware, 'w') as archive:
                archive.writestr('manifest.json', '{}')
            first, second = root / 'a.zip', root / 'b.zip'
            build_desktop(exe, firmware, first, 'linux')
            build_desktop(exe, firmware, second, 'linux')
            self.assertEqual(first.read_bytes(), second.read_bytes())
            with zipfile.ZipFile(first) as archive:
                self.assertEqual(archive.read('firmware.zip'), firmware.read_bytes())
                self.assertEqual(archive.getinfo('jollymaam').external_attr >> 16 & 0o777, 0o755)
            with self.assertRaises(ValueError):
                build_desktop(exe, firmware, firmware, 'linux')
            exe.write_bytes(b'invalid')
            before = first.read_bytes()
            with self.assertRaises(ValueError):
                build_desktop(exe, firmware, first, 'linux')
            self.assertEqual(before, first.read_bytes())

    def test_sdk_paths(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            sdk = root / 'sdk'
            (sdk / 'lib').mkdir(parents=True)
            (sdk / 'lib/libjollymaam.a').write_bytes(b'archive')
            output = root / 'sdk.zip'
            build_sdk(sdk, output)
            with zipfile.ZipFile(output) as archive:
                self.assertEqual(archive.namelist(), ['lib/libjollymaam.a'])
            with self.assertRaises(ValueError):
                build_sdk(sdk, sdk / 'bad.zip')
