import binascii
import struct
import zlib
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
from tools.jollymaam.firmware_package import build, PRODUCTS


class Package(c.Structure):
    _fields_ = [('data', c.c_void_p), ('size', c.c_size_t), ('directory', c.c_size_t), ('count', c.c_uint)]


class PackageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        lib = Path(cls.tmp.name) / 'package.so'
        subprocess.run([os.environ.get('CC', 'gcc'), '-std=c99', '-Wall', '-Wextra', '-Werror',
                        '-shared', '-fPIC', 'src/lib/package.c', 'src/lib/manifest.c',
                        'src/lib/sha256.c', 'src/lib/proto_jollymaam.c', 'src/lib/proto_8bitdo.c',
                        'src/lib/jieli_image/entry.c', 'src/lib/jieli_image/crypto.c', '-o', str(lib)], check=True)
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

    def test_sha256(self):
        self.lib.jm_sha256.argtypes = [c.c_void_p, c.c_size_t, c.c_void_p]
        for size in (0, 1, 55, 56, 63, 64, 65, 127, 128, 129, 1000000):
            data = bytes(i % 251 for i in range(size))
            digest = c.create_string_buffer(32)
            self.lib.jm_sha256(data, size, digest)
            self.assertEqual(digest.raw, hashlib.sha256(data).digest())

    def manifest_fixture(self, mutation=None):
        app = b'x'
        update = bytearray(0x120) + app
        struct.pack_into('<4sIIIII16s', update, 0, b'MAIN', 1, binascii.crc_hqx(app, 0),
                         0x16120, 1, 0x10000, b'U2C_WIRELESS')
        struct.pack_into('<I', update, 40, binascii.crc_hqx(update[4:40], 0))
        install = bytearray(32)
        install[8:12] = b'KNLT'
        struct.pack_into('<I', install, 24, 32)
        struct.pack_into('<I', install, 28, zlib.crc32(install[:28]) ^ 0xffffffff)
        install += bytes(4096 - 32) + update
        files = {'install.bin': bytes(install), 'update.bin': bytes(update)}
        target = dict(product='U2C_WIRELESS', board='test', chip='TLSR8208', model=0x301b,
                      preserve=[], artifacts=[])
        for role, path in [('install', 'install.bin'), ('update', 'update.bin')]:
            target['artifacts'].append(dict(role=role, path=path, size=len(files[path]),
                                            sha256=hashlib.sha256(files[path]).hexdigest()))
        manifest = dict(format='jollymaam-firmware', version=1, targets=[target])
        if mutation:
            mutation(manifest)
        stream = io.BytesIO()
        with zipfile.ZipFile(stream, 'w') as archive:
            archive.writestr('manifest.json', json.dumps(manifest))
            for path, data in files.items():
                archive.writestr(path, data)
        return stream.getvalue()

    def manifest_valid(self, data):
        buffer = c.create_string_buffer(data)
        package = Package()
        self.assertEqual(self.lib.jm_package_open(c.byref(package), buffer, len(data)), 0)
        # Caller storage is deliberately oversized and aligned for the C result.
        output = (c.c_uint64 * 1024)()
        self.lib.jm_manifest_open.argtypes = [c.c_void_p, c.POINTER(Package)]
        return self.lib.jm_manifest_open(output, c.byref(package))

    def test_manifest(self):
        self.assertEqual(self.manifest_valid(self.manifest_fixture()), 1)
        mutations = [lambda m: m.update(version=2),
                     lambda m: m['targets'][0]['artifacts'][0].update(sha256='0'*64),
                     lambda m: m['targets'][0].update(product='WRONG'),
                     lambda m: m['targets'][0].update(preserve=[dict(offset=0,size=4096)]),
                     lambda m: m['targets'].append(m['targets'][0]),
                     lambda m: m['targets'][0]['artifacts'].pop()]
        for mutate in mutations:
            self.assertEqual(self.manifest_valid(self.manifest_fixture(mutate)), 0)

    def test_malformed_json(self):
        valid = self.manifest_fixture()
        with zipfile.ZipFile(io.BytesIO(valid)) as archive:
            files = {name: archive.read(name) for name in archive.namelist()}
        text = files['manifest.json'].decode()
        bad = [text[:-1], text + ' garbage',
               text.replace('"version": 1', '"version": 1, "version": 1'),
               text.replace('"version": 1', '"version": 1.0'),
               text.replace('"version": 1', '"version": -1'),
               text.replace('"version": 1', '"version": 4294967296'),
               '{"extra":' + '['*20 + '0' + ']'*20 + '}',
               '{"extra":[' + ','.join('0' for _ in range(600)) + ']}']
        for broken in bad:
            stream = io.BytesIO()
            with zipfile.ZipFile(stream, 'w') as archive:
                for path, data in files.items():
                    archive.writestr(path, broken if path == 'manifest.json' else data)
            self.assertEqual(self.manifest_valid(stream.getvalue()), 0)

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
