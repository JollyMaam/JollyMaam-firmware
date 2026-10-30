import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from tools import test_firmware_package as package_tests

STUBS = r'''
#include "src/lib/transport_custom.h"
#include "src/lib/transport_hid.h"
#include "src/lib/transport_jieli.h"
#include "tools/jollymaam_update.h"
#include <stdio.h>
#include <string.h>
int jm_hid_custom_devices(jm_custom_visitor visit,void *user) {
 struct jm_identity id={0}; strcpy(id.product,"U2C_WIRELESS"); strcpy(id.build,"test-build");
 visit(user,"fake-custom",&id); return 1;
}
int jm_hid_enumerate_8bitdo(jm_hid_visit visit,void *user) { visit(user,"fake-boot",0x2dc8,0x3208); return 1; }
int jm_hid_8bitdo_model(const char *p,uint16_t *m) { (void)p; *m=0x301b; return 1; }
int jm_linux_jieli_candidates(jm_jieli_candidate_visitor v,void *u) { (void)v;(void)u;return 0; }
int jm_hid_custom_update(const char *p,const uint8_t expected_uid[16],const uint8_t *d,size_t n,void (*cb)(void*,size_t,size_t),void *u) {
 (void)d; (void)expected_uid; cb(u,n,n); printf("MOCK-CUSTOM:%s\n",p); return 0;
}
int jm_hid_upgrade_8bitdo(const char *p,const uint8_t *d,size_t n,void (*cb)(void*,unsigned,unsigned),void *u) {
 (void)d;(void)n;cb(u,1,1);printf("MOCK-BOOT:%s\n",p);return 0;
}
int jm_linux_jieli_upgrade(const char *p,const uint8_t *l,size_t s,const uint8_t *d,size_t n) {
 (void)p;(void)l;(void)s;(void)d;(void)n;puts("UNEXPECTED-JIELI");return -1;
}
int main(int argc,char **argv) { return argc==2 ? jollymaam_update_app(argv[1]) : 2; }
'''


class AppTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.temp.name)
        stub = cls.root / 'stub.c'
        stub.write_text(STUBS)
        cls.binary = cls.root / 'app'
        sources = ['tools/jollymaam_update.c', 'src/lib/package.c', 'src/lib/manifest.c',
                   'src/lib/sha256.c', 'src/lib/proto_jollymaam.c', 'src/lib/proto_8bitdo.c',
                   'src/lib/jieli_image/entry.c', 'src/lib/jieli_image/crypto.c']
        subprocess.run([os.environ.get('CC', 'gcc'), '-std=c99', '-Wall', '-Wextra', '-Werror',
                        '-I.', str(stub), *sources, '-o', str(cls.binary)], check=True)
        cls.package = cls.root / 'firmware.zip'
        cls.package.write_bytes(package_tests.PackageTests().manifest_fixture())

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_app(self, commands):
        return subprocess.run([str(self.binary), str(self.package)], input=commands,
                              capture_output=True, text=True, timeout=5)

    def test_cancel_eof_and_rescan(self):
        for commands in ('', 'q\n', '1\n\nq\n', '1\nno\nq\n', 'r\nq\n', '999\nq\n'):
            result = self.run_app(commands)
            self.assertEqual(result.returncode, 0)
            self.assertNotIn('MOCK-CUSTOM:', result.stdout)
            self.assertNotIn('MOCK-BOOT:', result.stdout)

    def test_custom_confirmation(self):
        result = self.run_app('1\nyes\nq\n')
        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stdout.count('MOCK-CUSTOM:fake-custom'), 1)
        self.assertNotIn('MOCK-BOOT:', result.stdout)
        self.assertIn('Current build: test-build', result.stdout)

    def test_bootloader_confirmation(self):
        result = self.run_app('2\nyes\nq\n')
        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stdout.count('MOCK-BOOT:fake-boot'), 1)
        self.assertNotIn('MOCK-CUSTOM:', result.stdout)
