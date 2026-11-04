"""Exercise the real JNI bridge using a host JVM and fake USB callbacks."""
import binascii
import hashlib
import io
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
import zipfile
import zlib


def package():
    """A package (libjollymaam's firmware.c format) for U2C_WIRELESS: a
    Telink install image and update.bin behind its MAIN header."""
    app = b'x'
    update = bytearray(b'\xff' * 0x120) + app
    struct.pack_into('<4sIIIII16s', update, 0, b'MAIN', 1, binascii.crc_hqx(app, 0), 0x16120, 1, 0x10000,
                     b'U2C_WIRELESS')
    struct.pack_into('<I', update, 40, binascii.crc_hqx(update[4:40], 0))
    install = bytearray(32)
    install[8:12] = b'KNLT'
    struct.pack_into('<I', install, 24, 32)
    struct.pack_into('<I', install, 28, zlib.crc32(install[:28]) ^ 0xffffffff)
    install += bytes(4096 - 32) + update
    files = {'install.bin': bytes(install), 'update.bin': bytes(update)}
    manifest = 'jollymaam-firmware 2\n' + ''.join(
        f'U2C_WIRELESS {role} {path} {len(files[path])} {hashlib.sha256(files[path]).hexdigest()}\n'
        for role, path in (('install', 'install.bin'), ('update', 'update.bin')))
    stream = io.BytesIO()
    with zipfile.ZipFile(stream, 'w') as archive:
        archive.writestr(zipfile.ZipInfo('manifest.txt'), manifest)
        for path, data in files.items():
            archive.writestr(zipfile.ZipInfo(path), data)
    return stream.getvalue()


class JniTests(unittest.TestCase):
    def test_native_bridge(self):
        java_home = os.environ.get('JAVA_HOME')
        if not java_home:
            self.skipTest('Run in the Android flake shell (or provide JAVA_HOME and javac)')
        with tempfile.TemporaryDirectory(prefix='jollymaam-jni-') as tmp:
            root = Path(tmp)
            sources = list(Path('src/lib').glob('*.c')) + list(Path('src/lib/jieli_image').glob('*.c'))
            subprocess.run([os.environ.get('CC', 'gcc'), '-std=c99', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            '-fPIC', '-shared', '-Isrc/lib', f'-I{java_home}/include', f'-I{java_home}/include/linux',
                            'android/app/src/main/cpp/jollymaam_jni.c', *map(str, sources), '-o', str(root/'libjollymaam-jni.so')], check=True)
            java = sorted(Path('android/tools/jni-test').rglob('*.java'))
            subprocess.run([f'{java_home}/bin/javac', '-d', str(root), *map(str, java)], check=True)
            (root / 'firmware.zip').write_bytes(package())
            result = subprocess.run([f'{java_home}/bin/java', '-Xcheck:jni', f'-Djava.library.path={root}',
                                     '-cp', str(root), 'dev.cyclone.padtool.JniTest', str(root / 'firmware.zip')],
                                    capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertNotIn('WARNING in native method', result.stdout + result.stderr)
            self.assertIn('exception tests passed', result.stdout)


if __name__ == '__main__':
    import sys
    sys.stdout.buffer.write(package()) if sys.argv[1:] == ['--package'] else unittest.main()
