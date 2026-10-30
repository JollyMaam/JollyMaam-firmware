import ctypes as c
import hashlib
import io
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
import warnings
import zipfile
from tools.firmware_package import build, PRODUCTS


class Package(c.Structure):
    _fields_ = [('data', c.c_void_p), ('size', c.c_size_t), ('directory', c.c_size_t), ('count', c.c_uint)]


class PackageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        lib = Path(cls.tmp.name) / 'package.so'
        subprocess.run([os.environ.get('CC', 'gcc'), '-std=c99', '-Wall', '-Wextra', '-Werror',
                        '-shared', '-fPIC', 'src/lib/package.c', '-o', str(lib)], check=True)
        cls.lib = c.CDLL(str(lib))
        cls.lib.jm_package_open.argtypes = [c.POINTER(Package), c.c_void_p, c.c_size_t]

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def check_zip(self, data):
        buffer = c.create_string_buffer(data)
        package = Package()
        return self.lib.jm_package_open(c.byref(package), buffer, len(data)), package.count

    def archive(self, names=('manifest.json',), compression=zipfile.ZIP_STORED):
        stream = io.BytesIO()
        with warnings.catch_warnings():
            warnings.simplefilter('ignore', UserWarning)
            with zipfile.ZipFile(stream, 'w', compression=compression) as archive:
                for name in names:
                    archive.writestr(name, b'payload')
        return stream.getvalue()

    def test_valid(self):
        self.assertEqual(self.check_zip(self.archive(('manifest.json', 'firmware/a.bin'))), (0, 2))

    def test_bad_archives(self):
        self.assertNotEqual(self.check_zip(b'')[0], 0)
        self.assertNotEqual(self.check_zip(self.archive()[:-1])[0], 0)
        self.assertEqual(self.check_zip(self.archive(compression=zipfile.ZIP_DEFLATED))[0], 2)
        self.assertEqual(self.check_zip(self.archive(('a', 'a')))[0], 4)
        data = self.archive().replace(b'payload', b'corrupt', 1)
        self.assertEqual(self.check_zip(data)[0], 3)
        for name in ('../a', '/a', 'a/../b', 'a\\b', 'a//b', 'C:a'):
            self.assertNotEqual(self.check_zip(self.archive((name,)))[0], 0)

    def test_reproducible_builder(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for board in PRODUCTS:
                (root / board).mkdir()
                for file in ('firmware.img', 'update.bin'):
                    (root / board / file).write_bytes(board.encode())
            for chip in ('br23', 'bd19'):
                directory = root / 'cpu' / chip / 'tools'
                directory.mkdir(parents=True)
                (directory / f'{chip}loader.bin').write_bytes(chip.encode())
            a, b = root / 'a.zip', root / 'b.zip'
            build(root, root, a)
            build(root, root, b)
            self.assertEqual(a.read_bytes(), b.read_bytes())
            self.assertEqual(self.check_zip(a.read_bytes()), (0, 13))
            with zipfile.ZipFile(a) as archive:
                manifest = json.loads(archive.read('manifest.json'))
                for target in manifest['targets']:
                    for item in target['artifacts'] + ([target['loader']] if 'loader' in target else []):
                        data = archive.read(item['path'])
                        self.assertEqual(len(data), item['size'])
                        self.assertEqual(hashlib.sha256(data).hexdigest(), item['sha256'])
