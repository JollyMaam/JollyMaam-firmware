"""Exercise the real JNI bridge using a host JVM and fake USB callbacks."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from tools.jollymaam import test_firmware_package as fixtures


class JniTests(unittest.TestCase):
    def test_native_bridge(self):
        java_home = os.environ.get('JAVA_HOME')
        if not java_home:
            self.skipTest('Run in the Android flake shell (or provide JAVA_HOME and javac)')
        with tempfile.TemporaryDirectory(prefix='jollymaam-jni-') as tmp:
            root = Path(tmp)
            sources = list(Path('src/lib/jieli_image').glob('*.c'))
            sources += list(Path('src/lib').glob('proto_*.c')) + list(Path('src/lib').glob('upgrade_*.c'))
            sources += [Path('src/lib') / name for name in ('restore.c', 'flash_jieli.c', 'manifest.c', 'package.c', 'sha256.c', 'session.c')]
            subprocess.run([os.environ.get('CC', 'gcc'), '-std=c99', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            '-fPIC', '-shared', '-Isrc/lib', f'-I{java_home}/include', f'-I{java_home}/include/linux',
                            'android/app/src/main/cpp/jollymaam_jni.c', *map(str, sources), '-o', str(root/'libjollymaam-jni.so')], check=True)
            java = sorted(Path('android/tools/jni-test').rglob('*.java'))
            subprocess.run([f'{java_home}/bin/javac', '-d', str(root), *map(str, java)], check=True)
            package = root / 'firmware.zip'
            package.write_bytes(fixtures.PackageTests().manifest_fixture())
            result = subprocess.run([f'{java_home}/bin/java', '-Xcheck:jni', f'-Djava.library.path={root}',
                                     '-cp', str(root), 'dev.cyclone.padtool.JniTest', str(package)],
                                    capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertNotIn('WARNING in native method', result.stdout + result.stderr)
            self.assertIn('exception tests passed', result.stdout)
